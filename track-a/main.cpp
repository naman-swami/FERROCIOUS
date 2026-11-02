#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <immintrin.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace {

constexpr unsigned kHours = 72;
constexpr size_t kChunk = 128 * 1024;
constexpr uint32_t kBlock = 1024;
constexpr size_t kWin = 16384;
constexpr unsigned kMaxThreads = 12;

inline uint64_t rd8(const char* p) { uint64_t w; memcpy(&w, p, 8); return w; }
inline uint64_t rd4(const char* p) { uint32_t w; memcpy(&w, p, 4); return w; }
inline uint64_t mum(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

inline uint64_t hash_ep(const char* p, uint32_t len) {
    constexpr uint64_t S0 = 0xA0761D6478BD642FULL, S1 = 0xE7037ED1A0B428DBULL;
    uint64_t seed = S0 ^ len, a, b;
    if (LIKELY(len >= 16)) {
        const char* q = p;
        uint32_t i = len;
        while (i > 16) { seed = mum(rd8(q) ^ S1, seed ^ rd8(q + 8)); q += 8; i -= 8; }
        a = rd8(p + len - 16); b = rd8(p + len - 8);
    } else if (len >= 8) {
        a = rd8(p); b = rd8(p + len - 8);
    } else if (len >= 4) {
        a = rd4(p); b = rd4(p + len - 4);
    } else if (len > 0) {
        uint8_t c0 = (uint8_t)p[0], c1 = (uint8_t)p[len >> 1], c2 = (uint8_t)p[len - 1];
        a = ((uint32_t)c0 << 16) | ((uint32_t)c1 << 8) | c2; b = 0;
    } else { a = b = 0; }
    return mum(mum(a ^ S0, b ^ seed), S1);
}

std::mutex g_mu;
std::unordered_map<std::string_view, uint32_t> g_map;
static char g_ep_arena[256 * 1024];
static size_t g_ep_arena_len = 0;
std::vector<std::string_view> g_eps;

struct Slot { uint64_t h; uint32_t len, gid; };
struct Spill { uint32_t hour, gid, lat, err; };

struct Worker {
    Slot* tbl = nullptr;
    uint32_t mask = 0, count = 0;
    uint32_t* cur[kHours];
    uint32_t* lim[kHours];
    std::vector<uint32_t*> blocks[kHours];
    uint32_t *bump = nullptr, *bump_end = nullptr;
    std::vector<Spill> spill;
    const char* nlv[kWin];

    void init() {
        constexpr uint32_t kInit = 256;
        mask = kInit - 1;
        tbl = (Slot*)calloc(kInit, sizeof(Slot));
        memset(cur, 0, sizeof cur);
        memset(lim, 0, sizeof lim);
    }

    void grow() {
        uint32_t old_cap = mask + 1, new_cap = old_cap * 2;
        Slot* old_tbl = tbl;
        tbl = (Slot*)calloc(new_cap, sizeof(Slot));
        mask = new_cap - 1;
        for (uint32_t i = 0; i < old_cap; ++i) {
            if (!old_tbl[i].h) continue;
            uint32_t j = (uint32_t)old_tbl[i].h & mask;
            while (tbl[j].h) j = (j + 1) & mask;
            tbl[j] = old_tbl[i];
        }
        free(old_tbl);
    }

    __attribute__((noinline)) uint32_t insert(const char* ep, uint32_t len, uint64_t h) {
        uint32_t gid;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_map.find(std::string_view(ep, len));
            if (it == g_map.end()) {
                char* copy_ptr = g_ep_arena + g_ep_arena_len;
                memcpy(copy_ptr, ep, len);
                g_ep_arena_len += len;
                std::string_view permanent_key(copy_ptr, len);
                gid = (uint32_t)g_eps.size();
                g_eps.push_back(permanent_key);
                g_map.emplace(permanent_key, gid);
            } else {
                gid = it->second;
            }
        }
        if ((count + 1) * 2 > mask + 1) grow();
        uint32_t i = (uint32_t)h & mask;
        while (tbl[i].h) i = (i + 1) & mask;
        tbl[i] = Slot{h, len, gid};
        ++count;
        return gid;
    }

    inline uint32_t gid_of(const char* ep, uint32_t len) {
        uint64_t h = hash_ep(ep, len);
        uint32_t i = (uint32_t)h & mask;
        for (;;) {
            const Slot& s = tbl[i];
            if (LIKELY(s.h == h && s.len == len)) return s.gid;
            if (s.h == 0) return insert(ep, len, h);
            i = (i + 1) & mask;
        }
    }

    __attribute__((noinline)) uint32_t* new_block(unsigned h) {
        if (bump == bump_end) {
            constexpr size_t kRegion = 1 << 20;
            void* m = malloc(kRegion);
            if (!m) abort();
            bump = (uint32_t*)m;
            bump_end = bump + kRegion / 4;
        }
        uint32_t* b = bump;
        bump += kBlock;
        blocks[h].push_back(b);
        lim[h] = b + kBlock;
        return b;
    }

    inline void put(uint32_t hour, uint32_t gid, uint32_t err, uint32_t lat) {
        if (UNLIKELY((lat >> 19) | (gid >> 12))) {
            spill.push_back(Spill{hour, gid, lat, err});
            return;
        }
        uint32_t* c = cur[hour];
        if (UNLIKELY(c == lim[hour])) c = new_block(hour);
        *c = lat | (err << 19) | (gid << 20);
        cur[hour] = c + 1;
    }

    size_t hour_count(unsigned h) const {
        size_t nb = blocks[h].size();
        return nb ? (nb - 1) * kBlock + (size_t)(cur[h] - blocks[h].back()) : 0;
    }

    inline bool fast_line(const char* p, const char* nl) {
        if (UNLIKELY(nl - p < 39)) return false;
        uint64_t x = rd8(p + 5);
        uint64_t y = rd8(p + 28);
        uint64_t w = rd8(nl - 8);

        uint32_t d1 = (uint32_t)((x >> 24) & 0xFF) - '0';
        uint32_t d2 = (uint32_t)((x >> 32) & 0xFF) - '0';
        uint32_t day = d1 * 10 + d2;
        uint32_t h1 = (uint32_t)((x >> 48) & 0xFF) - '0';
        uint32_t h2 = (uint32_t)((x >> 56) & 0xFF) - '0';
        uint32_t hr = h1 * 10 + h2;
        uint32_t hour = (((uint32_t)(x >> 8) & 0xFF) == '0' ? day - 30 : day + 1) * 24 + hr - 18;
        if (UNLIKELY(hour >= kHours)) return false;

        const char* q = p + 28;
        int mlen = (y & 0xFF) == ' ' ? 1 : ((y >> 8) & 0xFF) == ' ' ? 2 : ((y >> 16) & 0xFF) == ' ' ? 3 :
                   ((y >> 24) & 0xFF) == ' ' ? 4 : ((y >> 32) & 0xFF) == ' ' ? 5 : ((y >> 40) & 0xFF) == ' ' ? 6 : -1;
        if (UNLIKELY(mlen < 0)) return false;
        q += mlen;
        const char* ep = q;

        int sh = (w >> 56) == ' ' ? 56 : (w >> 48) == ' ' ? 48 : (w >> 40) == ' ' ? 40 :
                 (w >> 32) == ' ' ? 32 : (w >> 24) == ' ' ? 24 : (w >> 16) == ' ' ? 16 :
                 (w >>  8) == ' ' ?  8 : (w & 0xFF) == ' ' ? 0 : -1;
        if (UNLIKELY(sh < 0)) return false;
        const char* sp = (nl - 8) + (sh / 8);
        if (UNLIKELY(sp - 4 < ep)) return false;
        const char* ep_end = sp - 4;
        uint32_t err = (sp[-3] == '5');

        uint32_t lat = 0;
        for (const char* t = sp + 1; t < nl; ++t) {
            uint32_t dig = (uint32_t)(*t - '0');
            if (UNLIKELY(dig > 9)) return false;
            lat = lat * 10 + dig;
        }

        uint32_t gid = gid_of(ep, (uint32_t)(ep_end - ep));
        put(hour, gid, err, lat);
        return true;
    }

    void slow_line(const char* p, const char* end) {
        const char* nl = (const char*)memchr(p, '\n', end - p);
        if (!nl) return;
        const char* cur = p;
        if (cur + 28 >= nl) return;
        uint32_t day = (cur[8] - '0') * 10 + (cur[9] - '0');
        uint32_t hr = (cur[11] - '0') * 10 + (cur[12] - '0');
        uint32_t hour = (cur[6] == '0' ? day - 30 : day + 1) * 24 + hr - 18;
        if (hour >= kHours) return;

        const char* sp1 = (const char*)memchr(cur + 28, ' ', nl - (cur + 28));
        if (!sp1) return;
        const char* ep = sp1 + 1;
        const char* sp2 = (const char*)memchr(ep, ' ', nl - ep);
        if (!sp2) return;
        uint32_t elen = (uint32_t)(sp2 - ep);

        const char* sp3 = (const char*)memchr(sp2 + 1, ' ', nl - (sp2 + 1));
        if (!sp3) return;
        uint32_t err = (sp2[1] == '5');

        uint32_t lat = 0;
        for (const char* t = sp3 + 1; t < nl; ++t) {
            if (*t >= '0' && *t <= '9') lat = lat * 10 + (*t - '0');
        }
        uint32_t gid = gid_of(ep, elen);
        put(hour, gid, err, lat);
    }

    static inline const char** index_block(const char* p, const char** o) {
        __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
        __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
        __m256i m = _mm256_set1_epi8('\n');
        uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, m));
        uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, m));
        uint64_t mask = (uint64_t)m0 | ((uint64_t)m1 << 32);
        while (mask) {
            unsigned idx = __builtin_ctzll(mask);
            *o++ = p + idx;
            mask &= mask - 1;
        }
        return o;
    }

    inline const char* run_lines(const char* line, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const char* nl = nlv[i];
            if (!fast_line(line, nl)) slow_line(line, nl + 1);
            line = nl + 1;
        }
        return line;
    }

    void scan(const char* s, const char* e) {
        const char* line = s;
        const char* b = s;
        while (e - b >= 64) {
            size_t span = std::min<size_t>(kWin, (size_t)(e - b) & ~(size_t)63);
            const char** o = nlv;
            for (const char* we = b + span; b < we; b += 64) o = index_block(b, o);
            line = run_lines(line, (size_t)(o - nlv));
        }
        if (b < e) {
            alignas(64) char tail[64] = {};
            size_t rem = (size_t)(e - b);
            memcpy(tail, b, rem);
            const char** o = index_block(tail, nlv);
            size_t n = (size_t)(o - nlv);
            for (size_t i = 0; i < n; ++i) nlv[i] = b + (nlv[i] - tail);
            line = run_lines(line, n);
        }
        while (line < e) slow_line(line, e), line = (const char*)memchr(line, '\n', e - line) ? (const char*)memchr(line, '\n', e - line) + 1 : e;
    }
};

std::vector<Worker> g_workers;

constexpr auto kDig = [] {
    std::array<char, 200> a{};
    for (int i = 0; i < 100; ++i) { a[2 * i] = char('0' + i / 10); a[2 * i + 1] = char('0' + i % 10); }
    return a;
}();

inline char* put_u32(char* o, uint32_t v) {
    char t[10];
    int i = 10;
    while (v >= 100) {
        uint32_t q = v / 100, r = v - q * 100;
        i -= 2;
        memcpy(t + i, &kDig[2 * r], 2);
        v = q;
    }
    if (v >= 10) { i -= 2; memcpy(t + i, &kDig[2 * v], 2); } else t[--i] = char('0' + v);
    memcpy(o, t + i, (size_t)(10 - i));
    return o + (10 - i);
}

char g_hour_str[kHours][32];
std::vector<uint32_t> g_order;

struct HourOut { char* buf = nullptr; size_t len = 0; };
HourOut g_out[kHours];
int g_out_fd = -1;

void write_all(int fd, const char* b, size_t n) {
    while (n) {
#ifdef _WIN32
        int w = _write(fd, b, (unsigned)std::min<size_t>(n, 1 << 30));
#else
        ssize_t w = write(fd, b, n);
#endif
        if (w <= 0) break;
        b += w; n -= (size_t)w;
    }
}

void publish(unsigned h, char* buf, size_t len) {
    g_out[h] = HourOut{buf, len};
}

struct Scratch {
    std::vector<uint32_t> cnt, err, pos, A, res;
};

template <class F>
inline void for_records(unsigned h, F&& f) {
    for (Worker& w : g_workers) {
        const auto& bl = w.blocks[h];
        if (bl.empty()) continue;
        size_t full = bl.size() - 1;
        for (size_t i = 0; i < full; ++i) {
            const uint32_t* p = bl[i];
            for (uint32_t j = 0; j < kBlock; ++j) f(p[j]);
        }
        const uint32_t* p = bl[full];
        const uint32_t* lim = w.cur[h];
        while (p < lim) f(*p++);
    }
}

void process_hour(unsigned h, Scratch& sc) {
    const uint32_t G = (uint32_t)g_eps.size();
    if (sc.cnt.size() < G) { sc.cnt.resize(G); sc.err.resize(G); sc.pos.resize(G); sc.res.resize(3 * (size_t)G); }
    size_t hour_recs = 0;
    for (Worker& w : g_workers) hour_recs += w.hour_count(h) + w.spill.size();
    if (sc.A.size() < hour_recs) sc.A.resize(hour_recs);

    uint32_t* cnt = sc.cnt.data();
    uint32_t* err = sc.err.data();
    uint32_t* pos = sc.pos.data();
    uint32_t* A = sc.A.data();

    if (G) { memset(cnt, 0, G * 4); memset(err, 0, G * 4); }

    for_records(h, [&](uint32_t v) {
        uint32_t g = v >> 20;
        cnt[g]++;
        err[g] += (v >> 19) & 1;
    });
    for (Worker& w : g_workers) {
        for (const Spill& s : w.spill) {
            if (s.hour == h) { cnt[s.gid]++; err[s.gid] += s.err; }
        }
    }

    uint32_t run = 0;
    size_t cap = 64;
    for (uint32_t g : g_order) {
        pos[g] = run;
        run += cnt[g];
        if (cnt[g]) cap += g_eps[g].size() + 96;
    }

    for_records(h, [&](uint32_t v) {
        A[pos[v >> 20]++] = v & 0x7FFFF;
    });
    for (Worker& w : g_workers) {
        for (const Spill& s : w.spill) {
            if (s.hour == h) A[pos[s.gid]++] = s.lat;
        }
    }

    uint32_t* res = sc.res.data();
    for (uint32_t g : g_order) {
        uint32_t n = cnt[g];
        if (!n) continue;
        uint32_t* sl = A + (pos[g] - n);
        uint32_t p50_idx = (50ull * n + 99) / 100 - 1;
        uint32_t p95_idx = (95ull * n + 99) / 100 - 1;
        uint32_t p99_idx = (99ull * n + 99) / 100 - 1;

        if (n <= 64) {
            std::sort(sl, sl + n);
        } else {
            std::nth_element(sl, sl + p99_idx, sl + n);
            std::nth_element(sl, sl + p95_idx, sl + p99_idx);
            std::nth_element(sl, sl + p50_idx, sl + p95_idx);
        }
        res[3 * g] = sl[p50_idx];
        res[3 * g + 1] = sl[p95_idx];
        res[3 * g + 2] = sl[p99_idx];
    }

    char* buf = (char*)malloc(cap);
    char* o = buf;
    for (uint32_t g : g_order) {
        uint32_t n = cnt[g];
        if (!n) continue;
        uint32_t e = err[g];
        memcpy(o, g_hour_str[h], 20); o += 20; *o++ = ' ';
        std::string_view ep = g_eps[g];
        memcpy(o, ep.data(), ep.size()); o += ep.size(); *o++ = ' ';
        o = put_u32(o, n);              *o++ = ' ';
        o = put_u32(o, res[3 * g]);     *o++ = ' ';
        o = put_u32(o, res[3 * g + 1]); *o++ = ' ';
        o = put_u32(o, res[3 * g + 2]); *o++ = ' ';
        o = put_u32(o, e);              *o++ = ' ';
        o = put_u32(o, (uint32_t)((uint64_t)e * 1000000ull / n)); *o++ = '\n';
    }
    publish(h, buf, (size_t)(o - buf));
}

#ifdef _WIN32
inline ssize_t my_pread(HANDLE hFile, void* buf, size_t count, size_t offset) {
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf, (DWORD)count, &bytesRead, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        if (GetLastError() == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(hFile, &ov, &bytesRead, TRUE)) return -1;
            return (ssize_t)bytesRead;
        }
        return -1;
    }
    return (ssize_t)bytesRead;
}
#else
inline ssize_t my_pread(int fd, void* buf, size_t count, size_t offset) {
    return pread(fd, buf, count, offset);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::string dd = argc > 1 ? argv[1] : "/data", od = argc > 2 ? argv[2] : "/out";
    std::string in = dd + "/requests.log", outp = od + "/summary.txt";

    size_t total = 0;
#ifdef _WIN32
    HANDLE hFile = CreateFileA(in.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 1;
    LARGE_INTEGER liSize;
    GetFileSizeEx(hFile, &liSize);
    total = (size_t)liSize.QuadPart;
    _mkdir(od.c_str());
    g_out_fd = _open(outp.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0666);
#else
    mkdir(od.c_str(), 0755);
    int fd = open(in.c_str(), O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) != 0) return 1;
    total = (size_t)st.st_size;
    g_out_fd = open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
#endif

    if (g_out_fd < 0) return 1;

    for (unsigned h = 0; h < kHours; ++h) {
        unsigned t = 18 + h, d = t / 24, hh = t % 24;
        int mon = d < 2 ? 10 : 11, day = d == 0 ? 30 : d == 1 ? 31 : d == 2 ? 1 : 2;
        snprintf(g_hour_str[h], sizeof g_hour_str[h], "2026-%02d-%02dT%02u:00:00Z", mon, day, hh);
    }

    unsigned nt = std::thread::hardware_concurrency();
    if (nt == 0) nt = 6;
    nt = std::min(nt, kMaxThreads);
    const size_t nchunks = (total + kChunk - 1) / kChunk;
    if (nchunks < nt) nt = (unsigned)std::max<size_t>(nchunks, 1);
    g_workers.resize(nt);

    std::atomic<size_t> next_chunk{0};
    std::atomic<unsigned> next_hour{0};

    std::barrier bar(nt, [&]() noexcept {
        const uint32_t G = (uint32_t)g_eps.size();
        g_order.resize(G);
        for (uint32_t i = 0; i < G; ++i) g_order[i] = i;
        std::sort(g_order.begin(), g_order.end(), [](uint32_t a, uint32_t b) { return g_eps[a] < g_eps[b]; });
    });

    auto work = [&](unsigned tid) {
        Worker& w = g_workers[tid];
        w.init();
        alignas(64) char chunk_buf[kChunk + 4096];
        for (;;) {
            size_t c = next_chunk.fetch_add(1, std::memory_order_relaxed);
            if (c >= nchunks) break;
            size_t offset = c * kChunk;
            size_t to_read = std::min(kChunk + 4096, total - offset);
#ifdef _WIN32
            ssize_t n = my_pread(hFile, chunk_buf, to_read, offset);
#else
            ssize_t n = my_pread(fd, chunk_buf, to_read, offset);
#endif
            if (n <= 0) continue;
            const char* cur = chunk_buf;
            const char* limit = chunk_buf + n;
            if (c > 0) {
                while (cur < limit && *cur != '\n') cur++;
                if (cur < limit) cur++;
            }
            const char* end = cur;
            if (c + 1 < nchunks) {
                const char* target_end = chunk_buf + std::min((size_t)n, kChunk);
                while (target_end < limit && *target_end != '\n') target_end++;
                if (target_end < limit) target_end++;
                end = target_end;
            } else {
                end = limit;
            }
            if (cur < end) w.scan(cur, end);
        }
        bar.arrive_and_wait();
        Scratch sc;
        for (;;) {
            unsigned h = next_hour.fetch_add(1, std::memory_order_relaxed);
            if (h >= kHours) break;
            process_hour(h, sc);
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& th : pool) th.join();

    for (unsigned h = 0; h < kHours; ++h) {
        if (g_out[h].buf && g_out[h].len) {
            write_all(g_out_fd, g_out[h].buf, g_out[h].len);
            free(g_out[h].buf);
        }
    }

#ifdef _WIN32
    if (g_out_fd >= 0) _close(g_out_fd);
    CloseHandle(hFile);
#else
    if (g_out_fd >= 0) close(g_out_fd);
    close(fd);
    _exit(0);
#endif
    return 0;
}
