// FERROCIOUS - Track A (Ingest). C++20, standard library only, no third-party dependencies.
//
// Pipeline (threads are spawned once and reused):
//   phase 1  dynamic 128 KB chunks of the mmap'd log. Newlines are indexed 16 KB at a time with AVX2, then every
//            line is decoded independently (hour, endpoint id, error flag, latency) and appended as a 4-byte
//            record to a per-thread, per-hour block list. Nothing is sorted or counted here.
//   barrier  endpoints are sorted alphabetically once.
//   phase 2  dynamic hours: gather the hour's records from every thread and radix-sort them by (endpoint, latency)
//            (everything stays in L2), take nearest-rank percentiles, format the text and stream finished hours
//            to summary.txt strictly in hour order.
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
#include <sys/mman.h>
#include <unistd.h>
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace {

constexpr unsigned kHours = 72;
constexpr size_t kChunk = 1024 * 1024;
constexpr uint32_t kBlock = 1024;   // records per bucket block (4 KB)
constexpr size_t kWin = 16384;      // bytes indexed for newlines per window (stays in L1)
constexpr unsigned kMaxThreads = 12;

inline uint64_t rd8(const char* p) { uint64_t w; memcpy(&w, p, 8); return w; }
inline uint64_t rd4(const char* p) { uint32_t w; memcpy(&w, p, 4); return w; }
inline uint64_t mum(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

inline uint64_t hash_ep(const char* p, uint32_t len) {
    uint64_t h = len;
    while (len >= 8) { uint64_t v; memcpy(&v, p, 8); h = _mm_crc32_u64(h, v); p += 8; len -= 8; }
    if (len >= 4) { uint32_t v; memcpy(&v, p, 4); h = _mm_crc32_u32((uint32_t)h, v); p += 4; len -= 4; }
    while (len > 0) { h = _mm_crc32_u8((uint32_t)h, (uint8_t)*p++); len--; }
    return h ? h : 1;
}

std::mutex g_mu;
std::unordered_map<std::string_view, uint32_t> g_map;
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
    const char** nlv = nullptr;

    void init() {
        nlv = (const char**)malloc((kWin + 80) * sizeof(char*));
        mask = 4095;
        tbl = (Slot*)calloc(mask + 1, sizeof(Slot));
        for (unsigned h = 0; h < kHours; ++h) cur[h] = lim[h] = nullptr;
    }

    void grow() {
        uint32_t ncap = (mask + 1) * 2;
        Slot* nt = (Slot*)calloc(ncap, sizeof(Slot));
        for (uint32_t i = 0; i <= mask; ++i)
            if (tbl[i].h) {
                uint32_t j = (uint32_t)tbl[i].h & (ncap - 1);
                while (nt[j].h) j = (j + 1) & (ncap - 1);
                nt[j] = tbl[i];
            }
        free(tbl);
        tbl = nt;
        mask = ncap - 1;
    }

    __attribute__((noinline)) uint32_t insert(const char* ep, uint32_t len, uint64_t h) {
        uint32_t gid;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            std::string_view sv(ep, len);
            auto it = g_map.find(sv);
            if (it != g_map.end()) {
                gid = it->second;
            } else {
                gid = (uint32_t)g_eps.size();
                g_eps.push_back(sv);
                g_map.emplace(sv, gid);
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
            if (LIKELY(s.h == h && s.len == len)) {
                return s.gid;
            }
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
        uint32_t day = (uint32_t)((x >> 24) & 0xF) * 10 + (uint32_t)((x >> 32) & 0xF);
        uint32_t hr = (uint32_t)((x >> 48) & 0xF) * 10 + (uint32_t)((x >> 56) & 0xF);
        uint32_t hour = 24 * day + 744 * (uint32_t)((x >> 8) & 0xF) + hr - 738;

        uint64_t t = y ^ 0x2020202020202020ULL;
        uint64_t z = (t - 0x0101010101010101ULL) & ~t & 0x8080808080808080ULL;
        const char* ep = p + 28 + (__builtin_ctzll(z | (1ULL << 63)) >> 3) + 1;

        uint64_t t2 = w ^ 0x2020202020202020ULL;
        uint64_t z2 = ~(((t2 & 0x7F7F7F7F7F7F7F7FULL) + 0x7F7F7F7F7F7F7F7FULL) | t2 | 0x7F7F7F7F7F7F7F7FULL);
        uint32_t idx = (63u - (uint32_t)__builtin_clzll(z2 | 1)) >> 3;
        uint32_t nb = 7 - idx;
        const char* sp = nl - 8 + idx;
        const char* ep_end = sp - 4;

        uint64_t region = ~0ULL << (((8 - nb) * 8) & 63);
        uint64_t d = w ^ 0x3030303030303030ULL;
        uint64_t digit_bad = ((d + 0x7676767676767676ULL) | d) & 0x8080808080808080ULL & region;

        if (UNLIKELY((p[27] != ' ') | (hour >= kHours) | (z == 0) | (z2 == 0) | (nb == 0) | (ep_end <= ep) |
                     (*ep_end != ' ') | (digit_bad != 0)))
            return false;

        uint64_t v = w & region;
        v = ((v & 0x0F0F0F0F0F0F0F0FULL) * 2561) >> 8;
        v = ((v & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;
        v = ((v & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32;

        put(hour, gid_of(ep, (uint32_t)(ep_end - ep)), sp[-3] == '5', (uint32_t)v);
        return true;
    }

    const char* slow_line(const char* p, const char* end) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char* le = nl ? nl : end;
        const char* next = nl ? nl + 1 : end;
        if (le - p < 30) return next;
        uint32_t day = (uint32_t)(p[8] - '0') * 10 + (uint32_t)(p[9] - '0');
        uint32_t hr = (uint32_t)(p[11] - '0') * 10 + (uint32_t)(p[12] - '0');
        uint32_t hour = ((p[6] == '0') ? day - 30 : day + 1) * 24 + hr - 18;
        if (hour >= kHours) return next;
        const char* a = p + 28;
        const char* sp = (const char*)memchr(a, ' ', (size_t)(le - a));
        if (!sp) return next;
        const char* ep = sp + 1;
        const char* es = (const char*)memchr(ep, ' ', (size_t)(le - ep));
        if (!es) return next;
        const char* s = es + 1;
        if (s + 3 > le) return next;
        uint32_t err = s[0] == '5';
        s += 4;
        uint32_t lat = 0;
        while (s < le && (unsigned char)(*s - '0') <= 9) lat = lat * 10 + (uint32_t)(*s++ - '0');
        put(hour, gid_of(ep, (uint32_t)(es - ep)), err, lat);
        return next;
    }

    static inline uint64_t nl_mask64(const char* b) {
#ifdef __AVX2__
        const __m256i n = _mm256_set1_epi8('\n');
        uint32_t lo = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)b), n));
        uint32_t hi = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)(b + 32)), n));
        return (uint64_t)lo | ((uint64_t)hi << 32);
#else
        const __m128i n = _mm_set1_epi8('\n');
        uint64_t r = 0;
        for (int i = 0; i < 4; ++i)
            r |= (uint64_t)(uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(b + 16 * i)), n)) << (16 * i);
        return r;
#endif
    }

    static inline const char** index_block(const char* b, const char** o) {
        constexpr uint64_t kHi = 1ULL << 63;
        uint64_t m = nl_mask64(b);
        uint32_t cnt = (uint32_t)__builtin_popcountll(m);
        o[0] = b + __builtin_ctzll(m | kHi); m &= m - 1;
        o[1] = b + __builtin_ctzll(m | kHi); m &= m - 1;
        o[2] = b + __builtin_ctzll(m | kHi); m &= m - 1;
        o[3] = b + __builtin_ctzll(m | kHi); m &= m - 1;
        if (UNLIKELY(cnt > 4)) {
            o += 4;
            while (m) { *o++ = b + __builtin_ctzll(m); m &= m - 1; }
            return o;
        }
        return o + cnt;
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
        while (line < e) line = slow_line(line, e);
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



char g_hour_str[kHours][24];
std::vector<uint32_t> g_order;
size_t g_max_hour_n = 0;
size_t g_spill_n[kHours];

struct HourOut { char* buf = nullptr; size_t len = 0; };
HourOut g_out[kHours];
int g_out_fd = -1;

void write_all(int fd, const char* b, size_t n) {
    while (n) {
#ifdef _WIN32
        int w = _write(fd, b, (unsigned int)n);
#else
        ssize_t w = write(fd, b, n);
#endif
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        b += w;
        n -= (size_t)w;
    }
}

struct Scratch {
    std::vector<uint32_t> cnt, err, pos, A, res;
};

template <class F>
inline void for_records(unsigned h, F&& f) {
    for (Worker& w : g_workers) {
        const auto& bl = w.blocks[h];
        for (size_t j = 0, nb = bl.size(); j < nb; ++j) {
            const uint32_t* r = bl[j];
            size_t n = (j + 1 == nb) ? (size_t)(w.cur[h] - r) : kBlock;
            for (size_t k = 0; k < n; ++k) f(r[k]);
        }
    }
}

void process_hour(unsigned h, Scratch& sc) {
    const uint32_t G = (uint32_t)g_eps.size();
    if (sc.cnt.size() < G) { sc.cnt.resize(G); sc.err.resize(G); sc.pos.resize(G); sc.res.resize(3 * (size_t)G); }
    if (sc.A.size() < g_max_hour_n) { sc.A.resize(g_max_hour_n); }
    uint32_t *cnt = sc.cnt.data(), *err = sc.err.data(), *pos = sc.pos.data();
    uint32_t *A = sc.A.data();
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
    g_out[h] = HourOut{buf, (size_t)(o - buf)};
}

}  // namespace

int main(int argc, char** argv) {
    std::string dd = argc > 1 ? argv[1] : "/data", od = argc > 2 ? argv[2] : "/out";
    std::string in = dd + "/requests.log", outp = od + "/summary.txt";

#ifdef _WIN32
    HANDLE hFile = CreateFileA(in.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 1;
    LARGE_INTEGER liSize;
    GetFileSizeEx(hFile, &liSize);
    size_t total = liSize.QuadPart;
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    const char* data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, total);
    _mkdir(od.c_str());
    g_out_fd = _open(outp.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0666);
#else
    mkdir(od.c_str(), 0755);
    int fd = open(in.c_str(), O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) != 0) return 1;
    size_t total = (size_t)st.st_size;
    const char* data = nullptr;
    if (total) {
        void* m = mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) return 1;
        data = (const char*)m;
    }
    close(fd);
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
        for (Worker& w : g_workers)
            for (const Spill& s : w.spill) g_spill_n[s.hour]++;
        g_max_hour_n = 0;
        for (unsigned h = 0; h < kHours; ++h) {
            size_t n = g_spill_n[h];
            for (Worker& w : g_workers) n += w.hour_count(h);
            g_max_hour_n = std::max(g_max_hour_n, n);
        }
    });

    std::vector<size_t> chunk_offsets(nchunks + 1, 0);
    chunk_offsets[0] = 0;
    chunk_offsets[nchunks] = total;
    for (size_t c = 1; c < nchunks; ++c) {
        size_t pos = c * kChunk;
        size_t rem = std::min<size_t>(total - pos, 256);
        const void* nl = memchr(data + pos, '\n', rem);
        chunk_offsets[c] = nl ? (size_t)((const char*)nl - data) + 1 : total;
    }

    auto work = [&](unsigned tid) {
        Worker& w = g_workers[tid];
        w.init();
        for (;;) {
            size_t c = next_chunk.fetch_add(1, std::memory_order_relaxed);
            if (c >= nchunks) break;
            size_t s = chunk_offsets[c], e = chunk_offsets[c + 1];
            if (s < e) w.scan(data + s, data + e);
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
        if (g_out[h].buf) {
            write_all(g_out_fd, g_out[h].buf, g_out[h].len);
        }
    }

#ifdef _WIN32
    if (g_out_fd >= 0) _close(g_out_fd);
    UnmapViewOfFile(data);
    CloseHandle(hMap);
    CloseHandle(hFile);
#else
    if (g_out_fd >= 0) close(g_out_fd);
    _exit(0);
#endif
    return 0;
}
