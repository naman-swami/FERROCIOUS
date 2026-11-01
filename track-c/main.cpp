#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <cstring>
#include <cstdint>
#include <map>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <bit>
#include <memory>
#include "ferocious_common.hpp"

namespace {

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

inline void format_cents(int64_t cents, ferocious::OutBuf& out) {
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

inline int64_t parse_cents_strict(const char* p, const char* end) {
    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        p++;
    }
    int64_t whole = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        p++;
    }
    int64_t frac = 0;
    if (p < end && *p == '.') {
        p++;
        if (p < end && *p >= '0' && *p <= '9') frac = (p[0] - '0') * 10;
        if (p + 1 < end && *(p + 1) >= '0' && *(p + 1) <= '9') frac += (p[1] - '0');
    }
    int64_t total = whole * 100 + frac;
    return neg ? -total : total;
}

inline uint64_t hash_bytes(const char* p, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

static constexpr size_t POOL_CAP = 262144;
static constexpr size_t POOL_MASK = POOL_CAP - 1;

struct PoolSlot {
    std::atomic<uint32_t> state{0}; // 0 = empty, 1 = reserving, 2 = ready
    uint64_t hash = 0;
    uint32_t offset = 0;
    uint32_t len = 0;
    uint32_t name_id = 0;
};

static PoolSlot pool_table[POOL_CAP];
static char pool_arena[2 * 1024 * 1024]; // 2 MB fixed arena
static std::atomic<uint32_t> pool_arena_size{0};
static std::atomic<uint32_t> pool_next_id{0};

inline uint32_t get_or_register_name(const char* s, size_t len) {
    uint64_t h = hash_bytes(s, len);
    if (h == 0) h = 1;

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

        if (st == 2 && pool_table[idx].hash == h && pool_table[idx].len == len &&
            std::memcmp(pool_arena + pool_table[idx].offset, s, len) == 0) {
            return pool_table[idx].name_id;
        }

        idx = (idx + 1) & POOL_MASK;
    }
}

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

struct FastCountryMap {
    struct Slot {
        uint32_t code = 0;
        uint16_t cid = 0xFFFF;
    };
    Slot table[1024];

    FastCountryMap() {
        std::memset(table, 0, sizeof(table));
    }

    uint16_t get_or_insert(uint32_t code, const char* str_ptr, size_t clen, std::vector<std::string>& country_names) {
        size_t idx = (code * 0x9E3779B9u) & 1023;
        while (table[idx].code != 0) {
            if (table[idx].code == code) return table[idx].cid;
            idx = (idx + 1) & 1023;
        }
        table[idx].code = code;
        uint16_t cid = (uint16_t)country_names.size();
        table[idx].cid = cid;
        country_names.push_back(std::string(str_ptr, clen));
        return cid;
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

    get_or_register_name("", 0);

    // 1. Ingest regions.csv with FastCountryMap and 1M array
    std::memset(region_to_country, 0xFF, sizeof(region_to_country));
    std::vector<std::string> country_names;
    FastCountryMap cmap;
    uint16_t unmatched_cid = 0xFFFF;

    {
        ferocious::MappedFile rmf(regions_path.c_str());
        if (!rmf.ok) return 1;

        const char* p = static_cast<const char*>(rmf.data);
        const char* end = p + rmf.size;

        const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
        if (nl) p = nl + 1;

        while (p < end) {
            if (*p == '\r' || *p == '\n') { p++; continue; }
            uint32_t rid = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                rid = rid * 10 + (*p - '0');
                p++;
            }
            if (p < end && *p == ',') p++;
            const char* cstart = p;
            while (p < end && *p != '\r' && *p != '\n') p++;
            size_t clen = p - cstart;

            uint32_t code = 0;
            if (clen == 3) {
                code = (uint8_t)cstart[0] | ((uint8_t)cstart[1] << 8) | ((uint8_t)cstart[2] << 16);
            } else {
                for (size_t i = 0; i < clen && i < 4; ++i) code |= ((uint8_t)cstart[i] << (i * 8));
            }

            uint16_t cid = cmap.get_or_insert(code, cstart, clen, country_names);
            if (rid < 1000000) region_to_country[rid] = cid;

            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
        }

        unmatched_cid = (uint16_t)country_names.size();
        country_names.push_back("_UNMATCHED");
    }

    uint16_t num_countries = (uint16_t)country_names.size();

    // Allocate single shared global atomic bitset (Lock-free L3 resident)
    size_t total_words = (size_t)num_countries * BITSET_WORDS;
    auto global_seen = std::make_unique<std::atomic<uint64_t>[]>(total_words);

    // 2. Open records.csv
    ferocious::MappedFile mf(records_path.c_str());
    if (!mf.ok) return 1;

    const char* data = static_cast<const char*>(mf.data);
    size_t total_size = mf.size;

    size_t data_start = 0;
    if (total_size >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) {
        data_start = 3;
    }
    const char* hdr_nl = static_cast<const char*>(std::memchr(data + data_start, '\n', total_size - data_start));
    if (hdr_nl) data_start = (hdr_nl - data) + 1;

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    size_t chunk_bytes = (total_size - data_start) / num_threads;

    std::vector<size_t> chunk_starts(num_threads);
    std::vector<size_t> chunk_ends(num_threads);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        size_t start = data_start + tid * chunk_bytes;
        size_t end = (tid == num_threads - 1) ? total_size : data_start + (tid + 1) * chunk_bytes;

        if (tid > 0) {
            while (start + 1 < total_size && !(data[start] == '\r' && data[start + 1] == '\n')) {
                start++;
            }
            if (start + 1 < total_size) start += 2;
            if (start + 2 < total_size && (uint8_t)data[start] == 0xEF && (uint8_t)data[start+1] == 0xBB && (uint8_t)data[start+2] == 0xBF) {
                start += 3;
            }
        }

        if (tid < num_threads - 1) {
            while (end + 1 < total_size && !(data[end] == '\r' && data[end + 1] == '\n')) {
                end++;
            }
            if (end + 1 < total_size) end += 2;
        }

        chunk_starts[tid] = start;
        chunk_ends[tid] = end;
    }

    std::vector<std::vector<ThreadStats>> thread_stats(num_threads, std::vector<ThreadStats>(num_countries));

    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* p = data + chunk_starts[tid];
            const char* end = data + chunk_ends[tid];
            auto& stats = thread_stats[tid];

            char unescape_buf[512];

            while (p < end) {
                if (end - p >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
                    p += 3;
                }
                if (p >= end || *p == '\r' || *p == '\n') {
                    if (p < end) p++;
                    continue;
                }

                int64_t rec_id = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    rec_id = rec_id * 10 + (*p - '0');
                    p++;
                }
                if (p < end && *p == ',') p++;

                const char* name_ptr = nullptr;
                uint32_t name_len = 0;
                if (p < end && *p == '"') {
                    p++;
                    char* dest = unescape_buf;
                    while (p < end) {
                        if (*p == '"') {
                            if (p + 1 < end && *(p + 1) == '"') {
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
                } else {
                    const char* nstart = p;
                    while (p < end && *p != ',') p++;
                    name_ptr = nstart;
                    name_len = (uint32_t)(p - nstart);
                }
                if (p < end && *p == ',') p++;

                uint64_t note_len = 0;
                if (p < end && *p == '"') {
                    p++;
                    while (p < end) {
                        if (*p == '"') {
                            if (p + 1 < end && *(p + 1) == '"') {
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
                } else {
                    const char* ntstart = p;
                    while (p < end && *p != ',') p++;
                    note_len = (uint64_t)(p - ntstart);
                }
                if (p < end && *p == ',') p++;

                uint32_t region_id = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    region_id = region_id * 10 + (*p - '0');
                    p++;
                }
                if (p < end && *p == ',') p++;

                const char* amt_start = p;
                while (p < end && *p != '\r' && *p != '\n') p++;
                int64_t cents = parse_cents_strict(amt_start, p);

                if (p < end && *p == '\r') p++;
                if (p < end && *p == '\n') p++;

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

                uint32_t nid = get_or_register_name(name_ptr, name_len);
                size_t w_idx = (size_t)cid * BITSET_WORDS + (nid >> 6);
                uint64_t bit = 1ULL << (nid & 63);
                uint64_t old_val = global_seen[w_idx].load(std::memory_order_relaxed);
                if ((old_val & bit) == 0) {
                    global_seen[w_idx].fetch_or(bit, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    // 3. Merge stats and popcount global bitsets
    std::vector<CountryStats> final_stats(num_countries);
    for (uint16_t cid = 0; cid < num_countries; ++cid) {
        auto& fs = final_stats[cid];
        uint32_t distinct_cnt = 0;
        size_t base_idx = (size_t)cid * BITSET_WORDS;

        for (size_t k = 0; k < BITSET_WORDS; ++k) {
            distinct_cnt += (uint32_t)std::popcount(global_seen[base_idx + k].load(std::memory_order_relaxed));
        }
        fs.names_distinct = distinct_cnt;

        for (unsigned int tid = 0; tid < num_threads; ++tid) {
            const auto& ts = thread_stats[tid][cid];
            fs.count += ts.count;
            fs.id_sum += ts.id_sum;
            fs.amount_sum += ts.amount_sum;
            if (ts.amount_min < fs.amount_min) fs.amount_min = ts.amount_min;
            if (ts.amount_max > fs.amount_max) fs.amount_max = ts.amount_max;
            fs.name_bytes += ts.name_bytes;
            fs.note_bytes += ts.note_bytes;
        }
    }

    // 4. Output
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

    ferocious::OutBuf out(fp);

    for (uint16_t cid : active_cids) {
        const std::string& cname = country_names[cid];
        const auto& cs = final_stats[cid];
        out.puts(cname.data(), cname.size());
        out.put(' ');
        out.u64(cs.count);
        out.put(' ');
        out.u64(cs.id_sum);
        out.put(' ');
        format_cents(cs.amount_sum, out);
        out.put(' ');
        format_cents(cs.amount_min, out);
        out.put(' ');
        format_cents(cs.amount_max, out);
        out.put(' ');
        out.u64(cs.names_distinct);
        out.put(' ');
        out.u64(cs.name_bytes);
        out.put(' ');
        out.u64(cs.note_bytes);
        out.put('\n');
    }

    out.flush();
    fclose(fp);
    return 0;
}
