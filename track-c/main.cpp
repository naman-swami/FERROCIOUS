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
#include <bit>
#include <memory>
#include <chrono>
#include <immintrin.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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
    inline void puts(const char* s, size_t len) { for (size_t i = 0; i < len; ++i) put(s[i]); }
    inline void u64(uint64_t v) {
        char t[24]; int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
    }
};

struct MappedFile {
    void*  data = nullptr;
    size_t size = 0;
    bool   ok   = false;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE, hMap = NULL;
    explicit MappedFile(const char* path) {
        hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return; }
        size = (size_t)li.QuadPart;
        if (!size) { ok = true; return; }
        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        data = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        ok = (data != nullptr);
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }
#else
    explicit MappedFile(const char* path) {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return;
        struct stat st;
        if (::fstat(fd, &st) != 0) { ::close(fd); return; }
        size = (size_t)st.st_size;
        if (!size) { ok = true; ::close(fd); return; }
        data = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (data == MAP_FAILED) { data = nullptr; size = 0; return; }
        ok = true;
        if (size >= (1u << 21)) ::madvise(data, size, MADV_SEQUENTIAL | MADV_WILLNEED);
    }
    ~MappedFile() { if (data) ::munmap(data, size); }
#endif
};

struct CountryStats {
    uint64_t count = 0, name_bytes = 0, note_bytes = 0;
    int64_t id_sum = 0, amount_sum = 0, amount_min = INT64_MAX, amount_max = INT64_MIN;
    uint32_t names_distinct = 0;
};

inline void format_cents(int64_t cents, OutBuf& out) {
    if (cents < 0) { out.put('-'); cents = -cents; }
    out.u64(cents / 100);
    out.put('.');
    int rem = cents % 100;
    out.put('0' + (rem / 10));
    out.put('0' + (rem % 10));
}

inline int64_t parse_cents_fast(const char* p, const char* end) {
    bool neg = (*p == '-');
    p += neg;
    int64_t w = 0;
    while (p < end && (unsigned)(*p - '0') <= 9) w = w * 10 + (*p++ - '0');
    int64_t f = 0;
    if (p < end && *p == '.') {
        p++;
        if (p < end && (unsigned)(*p - '0') <= 9) f = (*p++ - '0') * 10;
        if (p < end && (unsigned)(*p - '0') <= 9) f += (*p++ - '0');
    }
    int64_t total = w * 100 + f;
    return neg ? -total : total;
}

inline uint64_t hash_bytes_fast(const char* p, size_t len) {
    uint64_t h = len;
    while (len >= 8) { uint64_t v; memcpy(&v, p, 8); h = _mm_crc32_u64(h, v); p += 8; len -= 8; }
    if (len >= 4) { uint32_t v; memcpy(&v, p, 4); h = _mm_crc32_u32((uint32_t)h, v); p += 4; len -= 4; }
    while (len > 0) { h = _mm_crc32_u8((uint32_t)h, (uint8_t)*p++); len--; }
    return h ? h : 1;
}

static constexpr size_t POOL_CAP = 262144, POOL_MASK = POOL_CAP - 1;
struct PoolSlot {
    std::atomic<uint32_t> state{0};
    uint32_t offset = 0, len = 0, name_id = 0;
    uint64_t hash = 0;
};
static PoolSlot pool_table[POOL_CAP];
static char pool_arena[2 * 1024 * 1024];
static std::atomic<uint32_t> pool_arena_size{0}, pool_next_id{0};

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
            _mm_pause();
            st = pool_table[idx].state.load(std::memory_order_acquire);
        }
        if (st == 2 && pool_table[idx].hash == h && pool_table[idx].len == len) {
            if (std::memcmp(pool_arena + pool_table[idx].offset, s, len) == 0) return pool_table[idx].name_id;
        }
        idx = (idx + 1) & POOL_MASK;
    }
}

struct NCache {
    static constexpr unsigned N = 16384, M = N - 1;
    uint64_t h[N]{};
    uint32_t len[N]{}, id[N]{};
    template <class F>
    inline uint32_t lookup(uint64_t hv, uint32_t n, F&& miss) {
        unsigned i = (unsigned)hv & M;
        if (h[i] == hv && len[i] == n) return id[i];
        uint32_t r = miss();
        h[i] = hv; len[i] = n; id[i] = r;
        return r;
    }
};

struct ThreadStats {
    uint64_t count = 0, name_bytes = 0, note_bytes = 0;
    int64_t id_sum = 0, amount_sum = 0, amount_min = INT64_MAX, amount_max = INT64_MIN;
};

static constexpr size_t BITSET_WORDS = 2048;
static uint16_t region_to_country[1000000];

} // namespace

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    std::vector<std::string> country_names;
    uint16_t unmatched_cid = 0;
    std::memset(region_to_country, 0xFF, sizeof(region_to_country));

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (!num_threads) num_threads = 6;
    num_threads = std::min(num_threads, 12u);

    {
        MappedFile rmf((data_dir + "/regions.csv").c_str());
        if (!rmf.ok) return 1;
        const char* rdata = (const char*)rmf.data;
        size_t rsize = rmf.size, r_start = 0;
        if (rsize >= 3 && (uint8_t)rdata[0] == 0xEF && (uint8_t)rdata[1] == 0xBB && (uint8_t)rdata[2] == 0xBF) r_start = 3;
        const char* r_nl = (const char*)std::memchr(rdata + r_start, '\n', rsize - r_start);
        if (r_nl) r_start = (r_nl - rdata) + 1;

        struct CSlot { uint32_t code = 0; uint16_t cid = 0xFFFF; };
        CSlot ctable[1024] = {};
        const char* p = rdata + r_start, *limit = rdata + rsize;

        while (p < limit) {
            if (*p == '\r' || *p == '\n') { p++; continue; }
            uint32_t rid = 0;
            while (p < limit && (unsigned)(*p - '0') <= 9) rid = rid * 10 + (*p++ - '0');
            if (p < limit && *p == ',') p++;
            const char* cstart = p;
            while (p < limit && *p != '\r' && *p != '\n') p++;
            size_t clen = p - cstart;

            uint32_t code = 0;
            for (size_t i = 0; i < clen && i < 4; ++i) code |= ((uint8_t)cstart[i] << (i * 8));
            size_t idx = (code * 0x9E3779B9u) & 1023;
            while (ctable[idx].code != 0 && ctable[idx].code != code) idx = (idx + 1) & 1023;
            uint16_t cid = ctable[idx].cid;
            if (cid == 0xFFFF) {
                cid = (uint16_t)country_names.size();
                country_names.push_back(std::string(cstart, clen));
                ctable[idx].code = code;
                ctable[idx].cid = cid;
            }
            if (rid < 1000000) region_to_country[rid] = cid;
            if (p < limit && *p == '\r') p++;
            if (p < limit && *p == '\n') p++;
        }
        unmatched_cid = (uint16_t)country_names.size();
        country_names.push_back("_UNMATCHED");
    }

    uint16_t num_countries = (uint16_t)country_names.size();
    size_t total_words = (size_t)num_countries * BITSET_WORDS;
    auto global_seen = std::make_unique<std::atomic<uint64_t>[]>(total_words);
    for (size_t i = 0; i < total_words; ++i) global_seen[i].store(0ULL, std::memory_order_relaxed);

    MappedFile mf((data_dir + "/records.csv").c_str());
    if (!mf.ok) return 1;
    const char* data = (const char*)mf.data;
    size_t total_size = mf.size, data_start = 0;
    if (total_size >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) data_start = 3;
    const char* hdr_nl = (const char*)std::memchr(data + data_start, '\n', total_size - data_start);
    if (hdr_nl) data_start = (hdr_nl - data) + 1;

    constexpr size_t CHUNK_SIZE = 256 * 1024;
    size_t num_chunks = (total_size - data_start + CHUNK_SIZE - 1) / CHUNK_SIZE;
    std::atomic<uint32_t> next_chunk{0};
    std::vector<ThreadStats> thread_stats(num_threads * num_countries);

    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            ThreadStats* stats = &thread_stats[tid * num_countries];
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
                    if (start + 2 < total_size && (uint8_t)data[start] == 0xEF && (uint8_t)data[start+1] == 0xBB && (uint8_t)data[start+2] == 0xBF) start += 3;
                }
                if (c + 1 < num_chunks) {
                    while (end + 1 < total_size && !(data[end] == '\r' && data[end + 1] == '\n')) end++;
                    if (end + 1 < total_size) end += 2;
                }

                const char* p = data + start, *limit = data + end;
                uint64_t in_q = 0;
                int cur_field_idx = 0;
                const char* cur_field_start = p;
                const char* id_s = p, *id_e = p, *name_s = p, *name_e = p, *note_s = p, *note_e = p, *reg_s = p, *reg_e = p;

                while (cur_field_start < limit && p < data + total_size) {
                    __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
                    __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));

                    uint32_t q0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, _mm256_set1_epi8('"')));
                    uint32_t q1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, _mm256_set1_epi8('"')));
                    uint64_t quotes = (uint64_t)q0 | ((uint64_t)q1 << 32);

                    uint32_t c0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, _mm256_set1_epi8(',')));
                    uint32_t c1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, _mm256_set1_epi8(',')));
                    uint64_t commas = (uint64_t)c0 | ((uint64_t)c1 << 32);

                    uint32_t n0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, _mm256_set1_epi8('\n')));
                    uint32_t n1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, _mm256_set1_epi8('\n')));
                    uint64_t nls = (uint64_t)n0 | ((uint64_t)n1 << 32);

                    __m128i pre = _mm_clmulepi64_si128(_mm_cvtsi64_si128((int64_t)quotes), _mm_set1_epi8(-1), 0x00);
                    uint64_t P = (uint64_t)_mm_cvtsi128_si64(pre);
                    if (in_q) P ^= ~0ULL;
                    in_q ^= (__builtin_popcountll(quotes) & 1);

                    uint64_t v_commas = commas & ~P, v_nls = nls & ~P;
                    uint64_t seps = v_commas | v_nls;

                    while (seps) {
                        unsigned b = (unsigned)__builtin_ctzll(seps);
                        const char* sep_ptr = p + b;
                        seps &= seps - 1;

                        if ((v_commas >> b) & 1) {
                            if (cur_field_idx == 0) { id_s = cur_field_start; id_e = sep_ptr; }
                            else if (cur_field_idx == 1) { name_s = cur_field_start; name_e = sep_ptr; }
                            else if (cur_field_idx == 2) { note_s = cur_field_start; note_e = sep_ptr; }
                            else if (cur_field_idx == 3) { reg_s = cur_field_start; reg_e = sep_ptr; }
                            cur_field_idx++;
                            cur_field_start = sep_ptr + 1;
                        } else {
                            const char* amt_s = cur_field_start, *amt_e = sep_ptr;
                            if (amt_e > amt_s && amt_e[-1] == '\r') amt_e--;

                            if (cur_field_idx == 4) {
                                while (id_s < id_e && (uint8_t)*id_s <= ' ') id_s++;
                                if (id_e - id_s >= 3 && (uint8_t)id_s[0] == 0xEF && (uint8_t)id_s[1] == 0xBB && (uint8_t)id_s[2] == 0xBF) id_s += 3;
                                int64_t rec_id = 0;
                                while (id_s < id_e && (unsigned)(*id_s - '0') <= 9) rec_id = rec_id * 10 + (*id_s++ - '0');

                                const char* name_ptr = "";
                                uint32_t name_len = 0;
                                if (name_s < name_e && *name_s == '"') {
                                    const char* ns = name_s + 1;
                                    const char* ne = (name_e > ns && name_e[-1] == '"') ? name_e - 1 : name_e;
                                    if (ne > ns) {
                                        const char* q = (const char*)std::memchr(ns, '"', ne - ns);
                                        if (!q) {
                                            name_ptr = ns;
                                            name_len = (uint32_t)(ne - ns);
                                        } else {
                                            char* dest = unescape_buf;
                                            const char* np = ns;
                                            while (np < ne) {
                                                if (*np == '"' && np + 1 < ne && *(np + 1) == '"') { *dest++ = '"'; np += 2; }
                                                else *dest++ = *np++;
                                            }
                                            name_ptr = unescape_buf;
                                            name_len = (uint32_t)(dest - unescape_buf);
                                        }
                                    }
                                } else {
                                    name_ptr = name_s;
                                    name_len = (uint32_t)(name_e - name_s);
                                }

                                uint64_t note_len = 0;
                                if (note_s < note_e && *note_s == '"') {
                                    const char* ns = note_s + 1;
                                    const char* ne = (note_e > ns && note_e[-1] == '"') ? note_e - 1 : note_e;
                                    if (ne > ns) {
                                        uint64_t raw_len = (uint64_t)(ne - ns);
                                        uint32_t q_cnt = 0;
                                        for (const char* cp = ns; cp < ne; ++cp) if (*cp == '"') q_cnt++;
                                        note_len = raw_len - (q_cnt / 2);
                                    }
                                } else {
                                    note_len = (uint64_t)(note_e - note_s);
                                }

                                uint32_t region_id = 0;
                                while (reg_s < reg_e && (unsigned)(*reg_s - '0') <= 9) region_id = region_id * 10 + (*reg_s++ - '0');

                                int64_t cents = parse_cents_fast(amt_s, amt_e);
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
                                uint32_t nid = ncache.lookup(nh, name_len, [&]() {
                                    return get_or_register_name_fast(name_ptr, name_len, nh);
                                });

                                size_t w_idx = (size_t)cid * BITSET_WORDS + (nid >> 6);
                                uint64_t bit = 1ULL << (nid & 63);
                                uint64_t old_val = global_seen[w_idx].load(std::memory_order_relaxed);
                                if (!(old_val & bit)) {
                                    global_seen[w_idx].fetch_or(bit, std::memory_order_relaxed);
                                }
                            }

                            cur_field_idx = 0;
                            cur_field_start = sep_ptr + 1;
                            if (cur_field_start + 3 <= data + total_size && (uint8_t)cur_field_start[0] == 0xEF && (uint8_t)cur_field_start[1] == 0xBB && (uint8_t)cur_field_start[2] == 0xBF) {
                                cur_field_start += 3;
                            }
                            if (cur_field_start >= limit) break;
                        }
                    }
                    p += 64;
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
            for (size_t cid = c_start; cid < c_end; ++cid) {
                auto& fs = final_stats[cid];
                size_t c_offset = cid * BITSET_WORDS;
                uint32_t distinct_cnt = 0;
                for (size_t k = 0; k < BITSET_WORDS; ++k) distinct_cnt += (uint32_t)std::popcount(global_seen[c_offset + k].load(std::memory_order_relaxed));
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

    FILE* fp = fopen((out_dir + "/aggregates.txt").c_str(), "wb");
    if (!fp) return 1;
    OutBuf out(fp);
    for (uint16_t cid : active_cids) {
        const auto& fs = final_stats[cid];
        const std::string& cname = country_names[cid];
        out.puts(cname.data(), cname.size()); out.put(' ');
        out.u64(fs.count); out.put(' ');
        out.u64(fs.id_sum); out.put(' ');
        format_cents(fs.amount_sum, out); out.put(' ');
        format_cents(fs.amount_min, out); out.put(' ');
        format_cents(fs.amount_max, out); out.put(' ');
        out.u64(fs.names_distinct); out.put(' ');
        out.u64(fs.name_bytes); out.put(' ');
        out.u64(fs.note_bytes); out.put('\n');
    }
    out.flush();
    fclose(fp);

#ifndef _WIN32
    _exit(0);
#endif
    return 0;
}
