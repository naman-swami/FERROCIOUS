#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <bit>
#include <memory>
#include <chrono>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

struct OutBuf {
    FILE* f;
    std::vector<char> b;
    size_t n = 0;
    explicit OutBuf(FILE* f) : f(f), b(1 << 20) {}
    ~OutBuf() { flush(); }
    void flush() { if (n && f) { fwrite(b.data(), 1, n, f); n = 0; } }
    inline void put(char c) { b[n++] = c; if (n == b.size()) flush(); }
    inline void puts(const char* s, size_t len) {
        for (size_t i = 0; i < len; ++i) put(s[i]);
    }
    inline void u64(uint64_t v) {
        char t[24]; int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
    }
};

struct MappedFile {
    void*    data = nullptr;
    size_t   size = 0;
    bool     ok   = false;
#ifdef _WIN32
    HANDLE   hFile = INVALID_HANDLE_VALUE;
    HANDLE   hMap  = NULL;
#endif

    explicit MappedFile(const char* path) {
#ifdef _WIN32
        hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return; }
        size = (size_t)li.QuadPart;
        if (size == 0) { ok = true; return; }
        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMap) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return; }
        data = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (!data) { CloseHandle(hMap); hMap = NULL; CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return; }
        ok = true;
#else
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return;
        struct stat st;
        if (::fstat(fd, &st) != 0) { ::close(fd); return; }
        size = (size_t)st.st_size;
        if (size == 0) { ok = true; ::close(fd); return; }
        data = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (data == MAP_FAILED) { data = nullptr; size = 0; return; }
        ok = true;
        if (size >= (1u << 21))
            ::madvise(data, size, MADV_SEQUENTIAL | MADV_WILLNEED);
#endif
    }

    ~MappedFile() {
#ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
        if (data) ::munmap(data, size);
#endif
    }
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

struct CountryStats {
    uint64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = INT64_MAX;
    int64_t amount_max = INT64_MIN;
    uint32_t names_distinct = 0;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
};

inline void format_cents(int64_t cents, OutBuf& out) {
    if (cents < 0) {
        out.put('-');
        cents = -cents;
    }
    out.u64(cents / 100);
    out.put('.');
    int rem = cents % 100;
    out.put('0' + (rem / 10));
    out.put('0' + (rem % 10));
}

inline int64_t parse_cents_fast(const char*& p, const char* end) {
    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        p++;
    }
    int64_t whole = 0;
    while (p < end && static_cast<unsigned char>(*p - '0') <= 9) {
        whole = whole * 10 + (*p - '0');
        p++;
    }
    int64_t frac = 0;
    if (p < end && *p == '.') {
        p++;
        if (p < end && static_cast<unsigned char>(*p - '0') <= 9) {
            frac = (*p - '0') * 10;
            p++;
        }
        if (p < end && static_cast<unsigned char>(*p - '0') <= 9) {
            frac += (*p - '0');
            p++;
        }
    }
    int64_t total = whole * 100 + frac;
    return neg ? -total : total;
}

inline uint64_t hash_bytes_fast(const char* p, size_t len) {
    uint64_t h = len * 0x517cc1b727220a95ULL;
    while (len >= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        h = (h ^ v) * 0x517cc1b727220a95ULL;
        p += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t v = 0;
        std::memcpy(&v, p, len);
        h = (h ^ v) * 0x517cc1b727220a95ULL;
    }
    return h;
}

static constexpr size_t POOL_CAP = 262144;
static constexpr size_t POOL_MASK = POOL_CAP - 1;

struct PoolSlot {
    std::atomic<uint32_t> state{0};
    uint32_t offset = 0;
    uint32_t len = 0;
    uint32_t name_id = 0;
    uint64_t hash = 0;
};

static PoolSlot pool_table[POOL_CAP];
static char pool_arena[2 * 1024 * 1024];
static std::atomic<uint32_t> pool_arena_size{0};
static std::atomic<uint32_t> pool_next_id{0};

inline uint32_t get_or_register_name_fast(const char* s, size_t len, uint64_t h) {
    size_t idx = h & POOL_MASK;
    while (true) {
        uint32_t st = pool_table[idx].state.load(std::memory_order_acquire);
        if (st == 0) {
            uint32_t expected = 0;
            if (pool_table[idx].state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                uint32_t off = pool_arena_size.fetch_add((uint32_t)len, std::memory_order_relaxed);
                std::memcpy(pool_arena + off, s, len);
                uint32_t nid = pool_next_id.fetch_add(1, std::memory_order_relaxed);
                pool_table[idx].offset = off;
                pool_table[idx].len = (uint32_t)len;
                pool_table[idx].name_id = nid;
                pool_table[idx].hash = h;
                pool_table[idx].state.store(2, std::memory_order_release);
                return nid;
            }
            st = pool_table[idx].state.load(std::memory_order_acquire);
        }

        while (st == 1) {
            #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
            #endif
            st = pool_table[idx].state.load(std::memory_order_acquire);
        }

        if (st == 2 && pool_table[idx].hash == h && pool_table[idx].len == len) {
            if (std::memcmp(pool_arena + pool_table[idx].offset, s, len) == 0) {
                return pool_table[idx].name_id;
            }
        }

        idx = (idx + 1) & POOL_MASK;
    }
}

struct NCache {
    static constexpr unsigned N = 2048, M = N - 1;
    uint64_t h[N]{};
    uint32_t len[N]{}, id[N]{};

    template <class F>
    inline uint32_t lookup(uint64_t hv, uint32_t n, F&& global_miss) {
        unsigned i = (unsigned)(hv >> 24) & M;
        if (h[i] == hv && len[i] == n) return id[i];
        uint32_t r = global_miss();
        h[i] = hv; len[i] = n; id[i] = r;
        return r;
    }
};

struct ThreadStats {
    uint64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = INT64_MAX;
    int64_t amount_max = INT64_MIN;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
};

static constexpr size_t BITSET_WORDS = 2048;

struct ThreadSafeCountryMap {
    struct Slot {
        std::atomic<uint32_t> code{0};
        uint16_t cid = 0xFFFF;
    };
    Slot table[1024];
    std::mutex mtx;

    ThreadSafeCountryMap() {
        for (size_t i = 0; i < 1024; ++i) {
            table[i].code.store(0, std::memory_order_relaxed);
            table[i].cid = 0xFFFF;
        }
    }

    uint16_t get_or_insert(uint32_t code, const char* str_ptr, size_t clen, std::vector<std::string>& country_names) {
        size_t idx = (code * 0x9E3779B9u) & 1023;
        while (true) {
            uint32_t c = table[idx].code.load(std::memory_order_acquire);
            if (c == code) return table[idx].cid;
            if (c == 0) {
                std::lock_guard<std::mutex> lock(mtx);
                c = table[idx].code.load(std::memory_order_relaxed);
                if (c == code) return table[idx].cid;
                if (c == 0) {
                    uint16_t cid = (uint16_t)country_names.size();
                    country_names.push_back(std::string(str_ptr, clen));
                    table[idx].cid = cid;
                    table[idx].code.store(code, std::memory_order_release);
                    return cid;
                }
            }
            idx = (idx + 1) & 1023;
        }
    }
};

static uint16_t region_to_country[1000000];

} // namespace

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    std::string regions_path = data_dir + "/regions.csv";
    std::string records_path = data_dir + "/records.csv";
    std::string aggregates_path = out_dir + "/aggregates.txt";

    std::vector<std::string> country_names;
    uint16_t unmatched_cid = 0;
    std::memset(region_to_country, 0xFF, sizeof(region_to_country));

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    {
        MappedFile rmf(regions_path.c_str());
        if (!rmf.ok) return 1;

        const char* rdata = static_cast<const char*>(rmf.data);
        size_t rsize = rmf.size;

        size_t r_start = 0;
        if (rsize >= 3 && (uint8_t)rdata[0] == 0xEF && (uint8_t)rdata[1] == 0xBB && (uint8_t)rdata[2] == 0xBF) {
            r_start = 3;
        }
        const char* r_nl = static_cast<const char*>(std::memchr(rdata + r_start, '\n', rsize - r_start));
        if (r_nl) r_start = (r_nl - rdata) + 1;

        ThreadSafeCountryMap cmap;
        constexpr size_t R_CHUNK_SIZE = 64 * 1024;
        size_t num_r_chunks = (rsize - r_start + R_CHUNK_SIZE - 1) / R_CHUNK_SIZE;
        std::atomic<uint32_t> next_r_chunk{0};

        std::vector<std::thread> r_workers;
        for (unsigned int tid = 0; tid < num_threads; ++tid) {
            r_workers.emplace_back([&]() {
                while (true) {
                    uint32_t c = next_r_chunk.fetch_add(1, std::memory_order_relaxed);
                    if (c >= num_r_chunks) break;

                    size_t start = r_start + (size_t)c * R_CHUNK_SIZE;
                    size_t end = std::min(r_start + ((size_t)c + 1) * R_CHUNK_SIZE, rsize);

                    if (c > 0) {
                        while (start + 1 < rsize && !(rdata[start] == '\r' && rdata[start + 1] == '\n')) start++;
                        if (start + 1 < rsize) start += 2;
                    }
                    if (c + 1 < num_r_chunks) {
                        while (end + 1 < rsize && !(rdata[end] == '\r' && rdata[end + 1] == '\n')) end++;
                        if (end + 1 < rsize) end += 2;
                    }

                    const char* p = rdata + start;
                    const char* limit = rdata + end;

                    while (p < limit) {
                        if (p >= limit || *p == '\r' || *p == '\n') {
                            if (p < limit) p++;
                            continue;
                        }

                        uint32_t rid = 0;
                        while (p < limit && static_cast<unsigned char>(*p - '0') <= 9) {
                            rid = rid * 10 + (*p - '0');
                            p++;
                        }
                        if (p < limit && *p == ',') p++;

                        const char* cstart = p;
                        while (p < limit && *p != '\r' && *p != '\n') p++;
                        size_t clen = p - cstart;

                        uint32_t code = 0;
                        if (clen == 2) {
                            code = (uint8_t)cstart[0] | ((uint8_t)cstart[1] << 8);
                        } else if (clen == 3) {
                            code = (uint8_t)cstart[0] | ((uint8_t)cstart[1] << 8) | ((uint8_t)cstart[2] << 16);
                        } else {
                            for (size_t i = 0; i < clen && i < 4; ++i) code |= ((uint8_t)cstart[i] << (i * 8));
                        }

                        uint16_t cid = cmap.get_or_insert(code, cstart, clen, country_names);
                        if (rid < 1000000) region_to_country[rid] = cid;

                        if (p < limit && *p == '\r') p++;
                        if (p < limit && *p == '\n') p++;
                    }
                }
            });
        }
        for (auto& w : r_workers) w.join();

        unmatched_cid = (uint16_t)country_names.size();
        country_names.push_back("_UNMATCHED");
    }

    uint16_t num_countries = (uint16_t)country_names.size();

    size_t words_per_thread = (size_t)num_countries * BITSET_WORDS;
    std::vector<uint64_t> thread_seen(num_threads * words_per_thread, 0ULL);

    MappedFile mf(records_path.c_str());
    if (!mf.ok) return 1;

    const char* data = static_cast<const char*>(mf.data);
    size_t total_size = mf.size;

    size_t data_start = 0;
    if (total_size >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) {
        data_start = 3;
    }
    const char* hdr_nl = static_cast<const char*>(std::memchr(data + data_start, '\n', total_size - data_start));
    if (hdr_nl) data_start = (hdr_nl - data) + 1;

    constexpr size_t CHUNK_SIZE = 256 * 1024;
    size_t num_chunks = (total_size - data_start + CHUNK_SIZE - 1) / CHUNK_SIZE;
    std::atomic<uint32_t> next_chunk{0};

    // Flattened 1D thread_stats: num_threads * num_countries
    std::vector<ThreadStats> thread_stats(num_threads * num_countries);

    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            ThreadStats* stats = &thread_stats[tid * num_countries];
            uint64_t* my_seen = &thread_seen[tid * words_per_thread];
            char unescape_buf[512];
            NCache ncache;

            while (true) {
                uint32_t c = next_chunk.fetch_add(1, std::memory_order_relaxed);
                if (c >= num_chunks) break;

                size_t start = data_start + (size_t)c * CHUNK_SIZE;
                size_t end = std::min(data_start + ((size_t)c + 1) * CHUNK_SIZE, total_size);

                if (c > 0) {
                    while (start + 1 < total_size && !(data[start] == '\r' && data[start + 1] == '\n')) start++;
                    if (start + 1 < total_size) start += 2;
                    if (start + 2 < total_size && (uint8_t)data[start] == 0xEF && (uint8_t)data[start+1] == 0xBB && (uint8_t)data[start+2] == 0xBF) {
                        start += 3;
                    }
                }
                if (c + 1 < num_chunks) {
                    while (end + 1 < total_size && !(data[end] == '\r' && data[end + 1] == '\n')) end++;
                    if (end + 1 < total_size) end += 2;
                }

                const char* p = data + start;
                const char* limit = data + end;

                while (p < limit) {
                    if (limit - p >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
                        p += 3;
                    }
                    if (p >= limit || *p == '\r' || *p == '\n') {
                        if (p < limit) p++;
                        continue;
                    }

                    int64_t rec_id = 0;
                    while (p < limit && static_cast<unsigned char>(*p - '0') <= 9) {
                        rec_id = rec_id * 10 + (*p - '0');
                        p++;
                    }
                    if (p < limit && *p == ',') p++;

                    const char* name_ptr = nullptr;
                    uint32_t name_len = 0;
                    if (p < limit && *p == '"') {
                        p++;
                        const char* q = static_cast<const char*>(std::memchr(p, '"', limit - p));
                        if (q && (q + 1 < limit && *(q + 1) == ',')) {
                            name_ptr = p;
                            name_len = (uint32_t)(q - p);
                            p = q + 2;
                        } else {
                            char* dest = unescape_buf;
                            while (p < limit) {
                                if (*p == '"') {
                                    if (p + 1 < limit && *(p + 1) == '"') {
                                        *dest++ = '"';
                                        p += 2;
                                    } else {
                                        p++;
                                        break;
                                    }
                                } else {
                                    *dest++ = *p++;
                                }
                            }
                            name_ptr = unescape_buf;
                            name_len = (uint32_t)(dest - unescape_buf);
                            if (p < limit && *p == ',') p++;
                        }
                    } else {
                        const char* nstart = p;
                        const char* comma = static_cast<const char*>(std::memchr(p, ',', limit - p));
                        if (comma) {
                            name_ptr = nstart;
                            name_len = (uint32_t)(comma - nstart);
                            p = comma + 1;
                        } else {
                            break;
                        }
                    }

                    uint64_t note_len = 0;
                    if (p < limit && *p == '"') {
                        p++;
                        const char* q = static_cast<const char*>(std::memchr(p, '"', limit - p));
                        if (q && (q + 1 < limit && *(q + 1) == ',')) {
                            note_len = (uint64_t)(q - p);
                            p = q + 2;
                        } else {
                            while (p < limit) {
                                if (*p == '"') {
                                    if (p + 1 < limit && *(p + 1) == '"') {
                                        note_len++;
                                        p += 2;
                                    } else {
                                        p++;
                                        break;
                                    }
                                } else {
                                    note_len++;
                                    p++;
                                }
                            }
                            if (p < limit && *p == ',') p++;
                        }
                    } else {
                        const char* ntstart = p;
                        const char* comma = static_cast<const char*>(std::memchr(p, ',', limit - p));
                        if (comma) {
                            note_len = (uint64_t)(comma - ntstart);
                            p = comma + 1;
                        } else {
                            break;
                        }
                    }

                    uint32_t region_id = 0;
                    while (p < limit && static_cast<unsigned char>(*p - '0') <= 9) {
                        region_id = region_id * 10 + (*p - '0');
                        p++;
                    }
                    if (p < limit && *p == ',') p++;

                    int64_t cents = parse_cents_fast(p, limit);

                    if (p < limit && *p == '\r') p++;
                    if (p < limit && *p == '\n') p++;

                    uint16_t cid = (region_id < 1000000) ? region_to_country[region_id] : 0xFFFF;
                    if (cid == 0xFFFF) cid = unmatched_cid;

                    auto& cs = stats[cid];
                    cs.count++;
                    cs.id_sum += rec_id;
                    cs.amount_sum += cents;
                    if (cents < cs.amount_min) cs.amount_min = cents;
                    if (cents > cs.amount_max) cs.amount_max = cents;
                    cs.name_bytes += name_len;
                    cs.note_bytes += note_len;

                    uint64_t nh = hash_bytes_fast(name_ptr, name_len);
                    if (nh == 0) nh = 1;

                    uint32_t nid = ncache.lookup(nh, name_len, [&]() {
                        return get_or_register_name_fast(name_ptr, name_len, nh);
                    });

                    size_t w_idx = (size_t)cid * BITSET_WORDS + (nid >> 6);
                    my_seen[w_idx] |= (1ULL << (nid & 63));
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    std::vector<CountryStats> final_stats(num_countries);
    std::vector<std::thread> agg_workers;
    size_t cids_per_thread = (num_countries + num_threads - 1) / num_threads;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        agg_workers.emplace_back([&, tid]() {
            size_t c_start = tid * cids_per_thread;
            size_t c_end = std::min((size_t)num_countries, (tid + 1) * cids_per_thread);
            alignas(64) uint64_t merged_buf[BITSET_WORDS];
            for (size_t cid = c_start; cid < c_end; ++cid) {
                auto& fs = final_stats[cid];
                std::memset(merged_buf, 0, sizeof(merged_buf));
                size_t c_offset = cid * BITSET_WORDS;

                for (unsigned int w_tid = 0; w_tid < num_threads; ++w_tid) {
                    const uint64_t* src = &thread_seen[w_tid * words_per_thread + c_offset];
                    for (size_t k = 0; k < BITSET_WORDS; k += 4) {
                        __m256i vm = _mm256_loadu_si256((const __m256i*)(merged_buf + k));
                        __m256i vs = _mm256_loadu_si256((const __m256i*)(src + k));
                        _mm256_storeu_si256((__m256i*)(merged_buf + k), _mm256_or_si256(vm, vs));
                    }
                }
                uint32_t distinct_cnt = 0;
                for (size_t k = 0; k < BITSET_WORDS; ++k) {
                    distinct_cnt += (uint32_t)std::popcount(merged_buf[k]);
                }
                fs.names_distinct = distinct_cnt;

                for (unsigned int w_tid = 0; w_tid < num_threads; ++w_tid) {
                    const auto& ts = thread_stats[w_tid * num_countries + cid];
                    fs.count += ts.count;
                    fs.id_sum += ts.id_sum;
                    fs.amount_sum += ts.amount_sum;
                    if (ts.amount_min < fs.amount_min) fs.amount_min = ts.amount_min;
                    if (ts.amount_max > fs.amount_max) fs.amount_max = ts.amount_max;
                    fs.name_bytes += ts.name_bytes;
                    fs.note_bytes += ts.note_bytes;
                }
            }
        });
    }
    for (auto& w : agg_workers) w.join();

#ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
#endif

    std::vector<uint16_t> active_cids;
    for (uint16_t cid = 0; cid < num_countries; ++cid) {
        if (final_stats[cid].count > 0) active_cids.push_back(cid);
    }
    std::sort(active_cids.begin(), active_cids.end(), [&](uint16_t a, uint16_t b) {
        return country_names[a] < country_names[b];
    });

    FILE* fp = fopen(aggregates_path.c_str(), "wb");
    if (!fp) return 1;

    OutBuf out(fp);
    for (uint16_t cid : active_cids) {
        const auto& fs = final_stats[cid];
        const std::string& cname = country_names[cid];
        out.puts(cname.data(), cname.size());
        out.put(' ');
        out.u64(fs.count);
        out.put(' ');
        out.u64(fs.id_sum);
        out.put(' ');
        format_cents(fs.amount_sum, out);
        out.put(' ');
        format_cents(fs.amount_min, out);
        out.put(' ');
        format_cents(fs.amount_max, out);
        out.put(' ');
        out.u64(fs.names_distinct);
        out.put(' ');
        out.u64(fs.name_bytes);
        out.put(' ');
        out.u64(fs.note_bytes);
        out.put('\n');
    }
    out.flush();
    fclose(fp);

    return 0;
}
