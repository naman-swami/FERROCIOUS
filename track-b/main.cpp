#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <thread>
#include <memory>
#include <immintrin.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using sv = std::string_view;
constexpr size_t CAP = 1 << 18;
struct Slot { uint64_t key; uint32_t id; };
Slot tab[CAP];
uint32_t nact;

sv map_file(const std::string &path) {
#ifdef _WIN32
  HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (hFile == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER li;
  if (!GetFileSizeEx(hFile, &li) || !li.QuadPart) { CloseHandle(hFile); return {}; }
  HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
  void* p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(hMap); CloseHandle(hFile);
  if (!p) return {};
  return {static_cast<const char *>(p), (size_t)li.QuadPart};
#else
  int fd = open(path.c_str(), O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !st.st_size) return {};
  void *p = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return {};
  madvise(p, st.st_size, MADV_WILLNEED | MADV_SEQUENTIAL);
  return {static_cast<const char *>(p), (size_t)st.st_size};
#endif
}

inline uint64_t word(const char *p, size_t n, const char *end) {
  uint64_t w = 0;
  memcpy(&w, p, p + 8 <= end ? 8 : n);
  w |= 0x2020202020202020ull;
  return n >= 8 ? w : w & ((1ull << (8 * n)) - 1);
}
inline uint64_t key_of(const char *s, size_t n, const char *end) {
  if (n <= 8) return word(s, n, end);
  uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
  for (size_t i = 0; i < n; i += 8) {
    h = (h ^ word(s + i, n - i, end)) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
  }
  return h | 1ull << 63;
}
inline size_t slot_of(uint64_t k) { return k * 0x9E3779B97F4A7C15ull >> 46; }
uint32_t add(uint64_t k) {
  size_t s = slot_of(k);
  for (; tab[s].key; s = (s + 1) & (CAP - 1))
    if (tab[s].key == k) return tab[s].id;
  tab[s] = {k, nact};
  return nact++;
}

inline uint32_t tokmask(const char *p) {
  auto v = [](int c) { return _mm256_set1_epi8((char)c); };
  __m256i c = _mm256_loadu_si256((const __m256i *)p);
  __m256i l = _mm256_or_si256(c, v(0x20));
  __m256i a = _mm256_and_si256(_mm256_cmpgt_epi8(l, v('a' - 1)), _mm256_cmpgt_epi8(v('z' + 1), l));
  __m256i d = _mm256_and_si256(_mm256_cmpgt_epi8(c, v('0' - 1)), _mm256_cmpgt_epi8(v('9' + 1), c));
  return _mm256_movemask_epi8(_mm256_or_si256(a, d));
}

template <class F> void scan_line(const char *p, const char *le, const char *end, F &&f) {
  const char *os = nullptr;
  bool open = false;
  uint64_t prev = 0;
  alignas(64) char buf[128];
  for (; p < le; p += 64) {
    const char *b = p;
    if (p + 64 > end) {
      memset(buf, 0, sizeof buf);
      memcpy(buf, p, end - p);
      b = buf;
    }
    uint64_t m = (uint64_t)tokmask(b) | (uint64_t)tokmask(b + 32) << 32;
    if (le - p < 64) m &= (1ull << (le - p)) - 1;
    uint64_t sh = m << 1 | prev, starts = m & ~sh, ends = ~m & sh;
    prev = m >> 63;
    for (;;) {
      if (open) {
        if (!ends) break;
        f(os, p + __builtin_ctzll(ends) - os);
        ends &= ends - 1;
        open = false;
      } else {
        if (!starts) break;
        os = p + __builtin_ctzll(starts);
        starts &= starts - 1;
        open = true;
      }
    }
  }
  if (open) f(os, le - os);
}

template <class F> uint32_t scan_chunk(sv file, const char *cur, const char *lim, uint32_t *off, F &&emit) {
  const char *end = file.data() + file.size();
  uint32_t id = 0, n = 0;
  uint64_t rk[16];
  size_t rs[16];
  while (cur < lim) {
    for (id = 0; cur < lim && (unsigned)(*cur - '0') <= 9; ++cur) id = id * 10 + (*cur - '0');
    if (cur < lim && *cur == '\t') ++cur;
    const char *nl = (const char *)memchr(cur, '\n', lim - cur);
    const char *le = nl ? nl : lim;
    if (off) off[id] = (uint32_t)(cur - file.data());
    n = id + 1;
    auto look = [&](uint64_t k, size_t s) {
      for (; tab[s].key; s = (s + 1) & (CAP - 1))
        if (tab[s].key == k) return emit(id, tab[s].id);
    };
    unsigned c = 0;
    scan_line(cur, le, end, [&](const char *s, size_t len) {
      uint64_t k = key_of(s, len, end);
      size_t sl = slot_of(k), i = c++ & 15;
      __builtin_prefetch(&tab[sl]);
      if (c > 16) look(rk[i], rs[i]);
      rk[i] = k; rs[i] = sl;
    });
    for (unsigned x = c > 16 ? c - 16 : 0; x < c; ++x) look(rk[x & 15], rs[x & 15]);
    cur = nl ? nl + 1 : lim;
  }
  return n;
}

template <class F> void run(unsigned n, F f) {
  std::vector<std::thread> th;
  for (unsigned i = 1; i < n; ++i) th.emplace_back(f, i);
  f(0);
  for (auto &t : th) t.join();
}

struct Post { const uint32_t *d; size_t n; const uint64_t *b; };

template <bool EQ, bool AO, bool BO>
size_t setop(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out) {
  size_t i = 0, j = 0, k = 0;
  while (i < na && j < nb) {
    uint32_t x = a[i], y = b[j];
    if (x == y) { if (EQ) out[k++] = x; ++i, ++j; }
    else if (x < y) { if (AO) out[k++] = x; ++i; }
    else { if (BO) out[k++] = y; ++j; }
  }
  if (AO) while (i < na) out[k++] = a[i++];
  if (BO) while (j < nb) out[k++] = b[j++];
  return k;
}

template <bool KEEP>
size_t filt(const uint32_t *a, size_t n, const uint64_t *b, uint32_t *out) {
  size_t k = 0;
  for (size_t i = 0; i < n; ++i) {
    out[k] = a[i];
    k += (b[a[i] >> 6] >> (a[i] & 63) & 1) == KEEP;
  }
  return k;
}

size_t gallop(const uint32_t *s, size_t ns, const uint32_t *l, size_t nl, uint32_t *out) {
  size_t j = 0, k = 0;
  for (size_t i = 0; i < ns && j < nl; ++i) {
    size_t step = 1, r = j;
    while (r < nl && l[r] < s[i]) { j = r; r += step; step <<= 1; }
    j = std::lower_bound(l + j, l + std::min(r, nl), s[i]) - l;
    if (j < nl && l[j] == s[i]) out[k++] = l[j++];
  }
  return k;
}

size_t inter(const uint32_t *a, size_t na, const Post &p, uint32_t *out) {
  if (p.b) return filt<true>(a, na, p.b, out);
  if (na * 8 < p.n) return gallop(a, na, p.d, p.n, out);
  return p.n * 8 < na ? gallop(p.d, p.n, a, na, out) : setop<1, 0, 0>(a, na, p.d, p.n, out);
}

size_t split(sv l, sv *w) {
  size_t n = 0, i = 0;
  auto sep = [&](size_t x) { return (unsigned char)l[x] <= ' ' || l[x] == '"'; };
  while (n < 32) {
    while (i < l.size() && sep(i)) ++i;
    if (i >= l.size()) break;
    size_t j = i;
    while (j < l.size() && !sep(j)) ++j;
    w[n++] = l.substr(i, j - i);
    i = j;
  }
  return n;
}

inline void append_u64(std::string &s, uint64_t v) {
  char buf[24]; int k = 0;
  do { buf[k++] = char('0' + (v % 10)); v /= 10; } while (v);
  while (k) s.push_back(buf[--k]);
}
} // namespace

int main(int argc, char **argv) {
  std::string in = argc > 1 ? argv[1] : "/data";
  std::string out = argc > 2 ? argv[2] : "/out";
  sv docs = map_file(in + "/docs.txt"), qs = map_file(in + "/queries.txt");
  if (docs.empty() || qs.empty()) return 1;
  const char *dend = docs.data() + docs.size(), *qend = qs.data() + qs.size();
  unsigned T = std::thread::hardware_concurrency();
  T = T ? std::min(T, 12u) : 6;

  std::vector<sv> lines;
  for (size_t p = 0, e; p < qs.size(); p = e + 1) {
    e = std::min(qs.find('\n', p), qs.size());
    lines.push_back(qs.substr(p, e - p));
    sv w[32];
    size_t n = split(lines.back(), w);
    for (size_t i = 1; i < n; ++i)
      if (w[i] != "AND" && w[i] != "OR" && w[i] != "NOT") add(key_of(w[i].data(), w[i].size(), qend));
  }

  std::vector<const char *> cs(T + 1, dend);
  cs[0] = docs.data();
  for (unsigned i = 1; i < T; ++i) {
    const char *p = docs.data() + docs.size() / T * i;
    p = (const char *)memchr(p, '\n', dend - p);
    cs[i] = p ? p + 1 : dend;
  }

    std::vector<uint32_t> cnt((size_t)T * nact), ndoc(T);
  run(T, [&](unsigned t) {
    std::vector<uint32_t> seen(nact, ~0u);
    uint32_t *c = cnt.data() + (size_t)t * nact;
    ndoc[t] = scan_chunk(docs, cs[t], cs[t + 1], nullptr, [&](uint32_t d, uint32_t k) {
      if (seen[k] != d) { seen[k] = d; ++c[k]; }
    });
  });
  size_t N = *std::max_element(ndoc.begin(), ndoc.end());

    std::vector<uint32_t> offs(nact + 1);
  uint32_t tot = 0;
  for (uint32_t k = 0; k < nact; ++k) {
    offs[k] = tot;
    for (unsigned t = 0; t < T; ++t) {
      uint32_t &c = cnt[(size_t)t * nact + k], v = c;
      c = tot;
      tot += v;
    }
  }
  offs[nact] = tot;

    auto csr = std::make_unique_for_overwrite<uint32_t[]>(tot);
  auto off = std::make_unique_for_overwrite<uint32_t[]>(N);
  run(T, [&](unsigned t) {
    std::vector<uint32_t> seen(nact, ~0u);
    uint32_t *w = cnt.data() + (size_t)t * nact;
    scan_chunk(docs, cs[t], cs[t + 1], off.get(), [&](uint32_t d, uint32_t k) {
      if (seen[k] != d) { seen[k] = d; csr[w[k]++] = d; }
    });
  });

    size_t W = (N + 63) / 64, thr = N / 32 + 1, nb = 0;
  for (uint32_t k = 0; k < nact; ++k) nb += offs[k + 1] - offs[k] >= thr;
  uint64_t *arena = (uint64_t *)calloc(nb * W + 1, 8), *nbs = arena;
  std::vector<uint64_t *> bs(nact, nullptr);
  for (uint32_t k = 0; k < nact; ++k)
    if (offs[k + 1] - offs[k] >= thr) { bs[k] = nbs; nbs += W; }
  run(T, [&](unsigned t) {
    for (uint32_t k = t; k < nact; k += T)
      for (uint32_t i = offs[k]; bs[k] && i < offs[k + 1]; ++i)
        bs[k][csr[i] >> 6] |= 1ull << (csr[i] & 63);
  });

    auto answer = [&](sv line, std::string &o, uint32_t *buf0, uint32_t *buf1) {
    sv w[32];
    size_t n = split(line, w);
    const uint32_t *cur = nullptr;
    size_t cn = 0;
    uint32_t *dst = buf0, *alt = buf1;
    auto post = [&](sv t) {
      uint64_t k = key_of(t.data(), t.size(), qend);
      size_t s = slot_of(k);
      while (tab[s].key && tab[s].key != k) s = (s + 1) & (CAP - 1);
      uint32_t id = tab[s].id;
      return Post{csr.get() + offs[id], offs[id + 1] - offs[id], bs[id]};
    };
    if (n > 1 && w[0] == "PHRASE") {
      size_t k = n - 1;
      uint64_t tk[32];
      int pi[32] = {};
      Post ps[32];
      size_t ord[32];
      uint64_t lens = 0;
      for (size_t i = 0; i < k; ++i) {
        lens |= 1ull << w[i + 1].size();
        tk[i] = key_of(w[i + 1].data(), w[i + 1].size(), qend);
        ps[i] = post(w[i + 1]);
        ord[i] = i;
      }
      for (size_t i = 1, j = 0; i < k; ++i) {
        while (j && tk[i] != tk[j]) j = pi[j - 1];
        pi[i] = j += tk[i] == tk[j];
      }
      std::sort(ord, ord + k, [&](size_t a, size_t b) { return ps[a].n < ps[b].n; });
      cur = ps[ord[0]].d; cn = ps[ord[0]].n;
      for (size_t i = 1; i < k && cn; ++i) {
        cn = inter(cur, cn, ps[ord[i]], dst);
        cur = dst;
        std::swap(dst, alt);
      }
      size_t m = 0;
      for (size_t c = 0; c < cn; ++c) {
        const char *s = docs.data() + off[cur[c]];
        const char *le = (const char *)memchr(s, '\n', dend - s);
        size_t j = 0;
        scan_line(s, le ? le : dend, dend, [&](const char *t, size_t len) {
          if (j == k) return;
          if (len > 63 || !(lens >> len & 1)) { j = 0; return; }
          uint64_t x = key_of(t, len, dend);
          while (j && x != tk[j]) j = pi[j - 1];
          j += x == tk[j];
        });
        if (j == k) dst[m++] = cur[c];
      }
      cur = dst; cn = m;
    } else if (n > 1) {
      Post p = post(w[1]);
      cur = p.d; cn = p.n;
      for (size_t i = 2; i + 1 < n; i += 2) {
        Post q = post(w[i + 1]);
        size_t m;
        if (w[i] == "OR") {
          if (!q.n) continue;
          m = setop<1, 1, 1>(cur, cn, q.d, q.n, dst);
        } else if (w[i] == "AND") {
          m = cn && q.n ? inter(cur, cn, q, dst) : 0;
        } else {
          if (!cn || !q.n) continue;
          m = q.b ? filt<false>(cur, cn, q.b, dst) : setop<0, 1, 0>(cur, cn, q.d, q.n, dst);
        }
        cur = dst; cn = m;
        std::swap(dst, alt);
      }
    }
    if (!cn) { o += "0 0\n"; return; }
    uint64_t sum = 0;
    size_t i = 0;
    __m256i vsum = _mm256_setzero_si256();
    for (; i + 8 <= cn; i += 8) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(cur + i));
      __m256i lo = _mm256_cvtepu32_epi64(_mm256_castsi256_si128(v));
      __m256i hi = _mm256_cvtepu32_epi64(_mm256_extracti128_si256(v, 1));
      vsum = _mm256_add_epi64(vsum, _mm256_add_epi64(lo, hi));
    }
    alignas(32) uint64_t sarr[4];
    _mm256_storeu_si256((__m256i *)sarr, vsum);
    sum = sarr[0] + sarr[1] + sarr[2] + sarr[3];
    for (; i < cn; ++i) sum += cur[i];
    append_u64(o, cn); o.push_back(' ');
    append_u64(o, sum);
    for (size_t j = 0; j < std::min<size_t>(cn, 10); ++j) {
      o.push_back(' ');
      append_u64(o, cur[j]);
    }
    o.push_back('\n');
  };

  std::vector<std::string> outs(T);
  size_t nq = lines.size();
  run(T, [&](unsigned t) {
    size_t s = nq * t / T, e = nq * (t + 1) / T;
    std::string &o = outs[t];
    o.reserve((e - s) * 48);
    auto b0 = std::make_unique<uint32_t[]>(N);
    auto b1 = std::make_unique<uint32_t[]>(N);
    for (size_t i = s; i < e; ++i) answer(lines[i], o, b0.get(), b1.get());
  });

    
  FILE *f = fopen((out + "/results.txt").c_str(), "wb");
  if (!f) return 1;
  for (unsigned t = 0; t < T; ++t) fwrite(outs[t].data(), 1, outs[t].size(), f);
  fclose(f);
  #ifndef _WIN32
  _exit(0);
#endif
  return 0;
}
