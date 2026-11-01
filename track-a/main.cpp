#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>
#include "ferocious_common.hpp"

namespace {

static const std::string HOURS[72] = {
    "2026-10-30T18:00:00Z", "2026-10-30T19:00:00Z", "2026-10-30T20:00:00Z",
    "2026-10-30T21:00:00Z", "2026-10-30T22:00:00Z", "2026-10-30T23:00:00Z",
    "2026-10-31T00:00:00Z", "2026-10-31T01:00:00Z", "2026-10-31T02:00:00Z",
    "2026-10-31T03:00:00Z", "2026-10-31T04:00:00Z", "2026-10-31T05:00:00Z",
    "2026-10-31T06:00:00Z", "2026-10-31T07:00:00Z", "2026-10-31T08:00:00Z",
    "2026-10-31T09:00:00Z", "2026-10-31T10:00:00Z", "2026-10-31T11:00:00Z",
    "2026-10-31T12:00:00Z", "2026-10-31T13:00:00Z", "2026-10-31T14:00:00Z",
    "2026-10-31T15:00:00Z", "2026-10-31T16:00:00Z", "2026-10-31T17:00:00Z",
    "2026-10-31T18:00:00Z", "2026-10-31T19:00:00Z", "2026-10-31T20:00:00Z",
    "2026-10-31T21:00:00Z", "2026-10-31T22:00:00Z", "2026-10-31T23:00:00Z",
    "2026-11-01T00:00:00Z", "2026-11-01T01:00:00Z", "2026-11-01T02:00:00Z",
    "2026-11-01T03:00:00Z", "2026-11-01T04:00:00Z", "2026-11-01T05:00:00Z",
    "2026-11-01T06:00:00Z", "2026-11-01T07:00:00Z", "2026-11-01T08:00:00Z",
    "2026-11-01T09:00:00Z", "2026-11-01T10:00:00Z", "2026-11-01T11:00:00Z",
    "2026-11-01T12:00:00Z", "2026-11-01T13:00:00Z", "2026-11-01T14:00:00Z",
    "2026-11-01T15:00:00Z", "2026-11-01T16:00:00Z", "2026-11-01T17:00:00Z",
    "2026-11-01T18:00:00Z", "2026-11-01T19:00:00Z", "2026-11-01T20:00:00Z",
    "2026-11-01T21:00:00Z", "2026-11-01T22:00:00Z", "2026-11-01T23:00:00Z",
    "2026-11-02T00:00:00Z", "2026-11-02T01:00:00Z", "2026-11-02T02:00:00Z",
    "2026-11-02T03:00:00Z", "2026-11-02T04:00:00Z", "2026-11-02T05:00:00Z",
    "2026-11-02T06:00:00Z", "2026-11-02T07:00:00Z", "2026-11-02T08:00:00Z",
    "2026-11-02T09:00:00Z", "2026-11-02T10:00:00Z", "2026-11-02T11:00:00Z",
    "2026-11-02T12:00:00Z", "2026-11-02T13:00:00Z", "2026-11-02T14:00:00Z",
    "2026-11-02T15:00:00Z", "2026-11-02T16:00:00Z", "2026-11-02T17:00:00Z"
};

inline uint32_t parse_hour_idx(const char* p) {
    uint8_t m_char = p[6]; // '0' for Oct (10), '1' for Nov (11)
    uint32_t day = (p[8] - '0') * 10 + (p[9] - '0');
    uint32_t hr  = (p[11] - '0') * 10 + (p[12] - '0');
    uint32_t day_offset = (m_char == '0') ? (day - 30) : (day + 1);
    return day_offset * 24 + hr - 18;
}

// 8-byte SWAR method skip
inline uint32_t skip_method_swar(const char* p) {
    uint64_t y;
    std::memcpy(&y, p, 8);
    uint64_t t = y ^ 0x2020202020202020ULL;
    uint64_t z = (t - 0x0101010101010101ULL) & ~t & 0x8080808080808080ULL;
    return (__builtin_ctzll(z) >> 3) + 1;
}

// 8-byte fast hash
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

static constexpr size_t LOCAL_DICT_CAP = 4096;
static constexpr size_t LOCAL_DICT_MASK = LOCAL_DICT_CAP - 1;

struct LocalDict {
    struct Entry {
        uint32_t hash = 0;
        uint32_t len = 0;
        const char* str = nullptr;
        uint16_t id = 0;
        uint16_t rank = 0;
    };
    std::vector<Entry> table;
    std::vector<std::string_view> list;
    std::vector<uint32_t> id_to_table_idx;

    LocalDict() : table(LOCAL_DICT_CAP) {
        list.reserve(2048);
        id_to_table_idx.reserve(2048);
    }

    inline uint16_t get_or_insert(const char* p, uint32_t len) {
        uint64_t h64 = hash_bytes_fast(p, len);
        uint32_t h = (uint32_t)(h64 ^ (h64 >> 32));
        if (h == 0) h = 1;

        size_t idx = h & LOCAL_DICT_MASK;
        while (true) {
            Entry& e = table[idx];
            if (e.hash == 0) {
                uint16_t id = (uint16_t)list.size();
                list.emplace_back(p, len);
                id_to_table_idx.push_back((uint32_t)idx);
                e.hash = h;
                e.len = len;
                e.str = p;
                e.id = id;
                return id;
            }
            if (e.hash == h && e.len == len && std::memcmp(e.str, p, len) == 0) {
                return e.id;
            }
            idx = (idx + 1) & LOCAL_DICT_MASK;
        }
    }

    inline void set_rank(uint16_t id, uint16_t rank) {
        uint32_t t_idx = id_to_table_idx[id];
        table[t_idx].rank = rank;
    }

    inline uint16_t get_rank_fast(const char* p, uint32_t len) const {
        uint64_t h64 = hash_bytes_fast(p, len);
        uint32_t h = (uint32_t)(h64 ^ (h64 >> 32));
        if (h == 0) h = 1;
        size_t idx = h & LOCAL_DICT_MASK;
        while (true) {
            const Entry& e = table[idx];
            if (e.hash == h && e.len == len && std::memcmp(e.str, p, len) == 0) {
                return e.rank;
            }
            idx = (idx + 1) & LOCAL_DICT_MASK;
        }
    }
};

inline uint32_t nearest_rank_percentile_slice(const uint32_t* sorted_vals, size_t n, uint32_t p) {
    size_t rank = (p * n + 99) / 100;
    return sorted_vals[rank - 1];
}

inline char* write_u32(char* p, uint32_t val) {
    char buf[16];
    char* cur = buf + 16;
    if (val == 0) {
        *p++ = '0';
        return p;
    }
    while (val > 0) {
        *--cur = '0' + (val % 10);
        val /= 10;
    }
    size_t len = (buf + 16) - cur;
    std::memcpy(p, cur, len);
    return p + len;
}

inline char* write_u64(char* p, uint64_t val) {
    char buf[32];
    char* cur = buf + 32;
    if (val == 0) {
        *p++ = '0';
        return p;
    }
    while (val > 0) {
        *--cur = '0' + (val % 10);
        val /= 10;
    }
    size_t len = (buf + 32) - cur;
    std::memcpy(p, cur, len);
    return p + len;
}

struct ThreadChunk {
    size_t start;
    size_t end;
};

} // namespace

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    std::string input_path = data_dir + "/requests.log";
    std::string output_path = out_dir + "/summary.txt";

    ferocious::MappedFile mf(input_path.c_str());
    if (!mf.ok) return 1;

    const char* data = static_cast<const char*>(mf.data);
    size_t total_size = mf.size;

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    constexpr size_t CHUNK_SIZE = 256 * 1024;
    size_t num_chunks = (total_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    std::atomic<uint32_t> next_chunk{0};

    std::vector<LocalDict> thread_dicts(num_threads);
    std::vector<std::vector<ThreadChunk>> thread_chunks(num_threads);

    constexpr size_t LOCAL_MAX_CELLS = 72 * 2048;
    std::vector<std::vector<uint32_t>> thread_local_counts(num_threads, std::vector<uint32_t>(LOCAL_MAX_CELLS, 0));
    std::vector<std::vector<uint32_t>> thread_local_errors(num_threads, std::vector<uint32_t>(LOCAL_MAX_CELLS, 0));

    // PASS 1: Work-stealing parse, count frequencies, record thread chunks
    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            auto& dict = thread_dicts[tid];
            auto& my_chunks = thread_chunks[tid];
            auto& counts = thread_local_counts[tid];
            auto& errors = thread_local_errors[tid];

            while (true) {
                uint32_t c = next_chunk.fetch_add(1, std::memory_order_relaxed);
                if (c >= num_chunks) break;

                size_t start = (size_t)c * CHUNK_SIZE;
                size_t end = std::min(start + CHUNK_SIZE, total_size);

                if (c > 0) {
                    while (start < total_size && data[start] != '\n') start++;
                    if (start < total_size) start++;
                }
                if (c + 1 < num_chunks) {
                    while (end < total_size && data[end] != '\n') end++;
                    if (end < total_size) end++;
                }

                my_chunks.push_back({start, end});

                const char* cur = data + start;
                const char* limit = data + end;

                while (cur < limit) {
                    const char* line_start = cur;
                    if (cur + 28 >= limit) break;

                    uint32_t hour_idx = parse_hour_idx(line_start);

                    // SWAR Method Skip
                    const char* p = line_start + 28;
                    p += skip_method_swar(p);

                    // Endpoint
                    const char* ep_start = p;
                    const char* sp = static_cast<const char*>(std::memchr(p, ' ', limit - p));
                    if (!sp) break;
                    uint32_t ep_len = (uint32_t)(sp - ep_start);

                    uint16_t ep_id = dict.get_or_insert(ep_start, ep_len);

                    // Status: sp[1] is '2'/'3'/'4'/'5'
                    uint8_t is_err = (sp[1] == '5');

                    size_t cell = (size_t)hour_idx * 2048 + ep_id;
                    counts[cell]++;
                    if (is_err) errors[cell]++;

                    // Advance past latency to next line
                    const char* p_lat = sp + 5;
                    while (p_lat < limit && *p_lat != '\n') p_lat++;
                    cur = p_lat + 1;
                }
            }
        });
    }

    for (auto& w : workers) w.join();
    workers.clear();

    // BARRIER: Build unified global endpoint dictionary & lexicographical order
    std::vector<std::string_view> all_endpoints;
    all_endpoints.reserve(2048);

    static constexpr size_t GLOBAL_MASK = 4095;
    struct GlobalSlot {
        uint32_t hash = 0;
        uint32_t len = 0;
        const char* str = nullptr;
        uint16_t id = 0;
    };
    std::vector<GlobalSlot> global_table(4096);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        for (const auto& sv : thread_dicts[tid].list) {
            uint64_t h64 = hash_bytes_fast(sv.data(), sv.size());
            uint32_t h = (uint32_t)(h64 ^ (h64 >> 32));
            if (h == 0) h = 1;

            size_t idx = h & GLOBAL_MASK;
            while (true) {
                auto& s = global_table[idx];
                if (s.hash == 0) {
                    uint16_t new_id = (uint16_t)all_endpoints.size();
                    all_endpoints.push_back(sv);
                    s.hash = h;
                    s.len = (uint32_t)sv.size();
                    s.str = sv.data();
                    s.id = new_id;
                    break;
                }
                if (s.hash == h && s.len == sv.size() && std::memcmp(s.str, sv.data(), sv.size()) == 0) {
                    break;
                }
                idx = (idx + 1) & GLOBAL_MASK;
            }
        }
    }

    uint16_t num_global_eps = (uint16_t)all_endpoints.size();

    // Alphabetical endpoint sorting
    std::vector<uint16_t> sorted_order(num_global_eps);
    for (uint16_t i = 0; i < num_global_eps; ++i) sorted_order[i] = i;
    std::sort(sorted_order.begin(), sorted_order.end(), [&](uint16_t a, uint16_t b) {
        return all_endpoints[a] < all_endpoints[b];
    });

    std::vector<uint16_t> global_to_rank(num_global_eps);
    for (uint16_t r = 0; r < num_global_eps; ++r) {
        global_to_rank[sorted_order[r]] = r;
    }

    // Remap tables: thread_dict_id -> rank, and update LocalDict with ranks
    std::vector<std::vector<uint16_t>> thread_remap(num_threads);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        auto& remap = thread_remap[tid];
        auto& dict = thread_dicts[tid];
        const auto& list = dict.list;
        remap.resize(list.size());
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& sv = list[i];
            uint64_t h64 = hash_bytes_fast(sv.data(), sv.size());
            uint32_t h = (uint32_t)(h64 ^ (h64 >> 32));
            if (h == 0) h = 1;
            size_t idx = h & GLOBAL_MASK;
            while (true) {
                const auto& s = global_table[idx];
                if (s.hash == h && s.len == sv.size() && std::memcmp(s.str, sv.data(), sv.size()) == 0) {
                    uint16_t rank = global_to_rank[s.id];
                    remap[i] = rank;
                    dict.set_rank((uint16_t)i, rank);
                    break;
                }
                idx = (idx + 1) & GLOBAL_MASK;
            }
        }
    }

    // Direct Accumulation of cell totals and errors (ZERO intermediate matrix allocations)
    size_t total_cells = 72 * (size_t)num_global_eps;
    std::vector<uint32_t> cell_totals(total_cells, 0);
    std::vector<uint32_t> cell_errors(total_cells, 0);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        const auto& remap = thread_remap[tid];
        const auto& counts = thread_local_counts[tid];
        const auto& errors = thread_local_errors[tid];

        size_t num_local = remap.size();
        for (uint32_t h = 0; h < 72; ++h) {
            size_t local_base = (size_t)h * 2048;
            size_t global_base = (size_t)h * num_global_eps;
            for (size_t id = 0; id < num_local; ++id) {
                uint32_t cnt = counts[local_base + id];
                if (cnt > 0) {
                    uint16_t rank = remap[id];
                    size_t c = global_base + rank;
                    cell_totals[c] += cnt;
                    cell_errors[c] += errors[local_base + id];
                }
            }
        }
    }

    // Immediately free thread_local buffers BEFORE allocating flat latencies array!
    thread_local_counts.clear();
    thread_local_counts.shrink_to_fit();
    thread_local_errors.clear();
    thread_local_errors.shrink_to_fit();
    thread_remap.clear();
    thread_remap.shrink_to_fit();

    // Compute Prefix sums
    std::vector<uint32_t> cell_offsets(total_cells + 1, 0);
    std::vector<std::atomic<uint32_t>> cell_heads(total_cells);

    uint32_t running_total = 0;
    for (size_t c = 0; c < total_cells; ++c) {
        cell_offsets[c] = running_total;
        cell_heads[c].store(running_total, std::memory_order_relaxed);
        running_total += cell_totals[c];
    }
    cell_offsets[total_cells] = running_total;
    uint32_t total_records = running_total;

    // Single 12.4 MB flat contiguous array locked in L3 cache
    std::vector<uint32_t> all_latencies(total_records);

    // PASS 2: Replay recorded chunks with lock-free atomic cell_head fetch_add
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const auto& dict = thread_dicts[tid];
            const auto& my_chunks = thread_chunks[tid];

            for (const auto& chk : my_chunks) {
                const char* cur = data + chk.start;
                const char* limit = data + chk.end;

                while (cur < limit) {
                    const char* line_start = cur;
                    if (cur + 28 >= limit) break;

                    uint32_t hour_idx = parse_hour_idx(line_start);

                    // SWAR Method Skip
                    const char* p = line_start + 28;
                    p += skip_method_swar(p);

                    // Endpoint
                    const char* ep_start = p;
                    const char* sp = static_cast<const char*>(std::memchr(p, ' ', limit - p));
                    if (!sp) break;
                    uint32_t ep_len = (uint32_t)(sp - ep_start);

                    uint16_t rank = dict.get_rank_fast(ep_start, ep_len);

                    // Latency
                    const char* p_lat = sp + 5;
                    uint32_t latency = 0;
                    while (p_lat < limit && *p_lat != '\n') {
                        latency = latency * 10 + (*p_lat - '0');
                        p_lat++;
                    }
                    cur = p_lat + 1;

                    size_t cell_idx = (size_t)hour_idx * num_global_eps + rank;
                    uint32_t pos = cell_heads[cell_idx].fetch_add(1, std::memory_order_relaxed);
                    all_latencies[pos] = latency;
                }
            }
        });
    }

    for (auto& w : workers) w.join();
    workers.clear();

    thread_chunks.clear();
    thread_chunks.shrink_to_fit();
    std::vector<std::atomic<uint32_t>>().swap(cell_heads);

    // PASS 3: Parallel sorting & output formatting per hour chunk
    std::vector<std::string> thread_output_strings(num_threads);
    uint32_t hours_per_thread = (72 + num_threads - 1) / num_threads;

    for (unsigned int h_tid = 0; h_tid < num_threads; ++h_tid) {
        workers.emplace_back([&, h_tid]() {
            uint32_t h_start = h_tid * hours_per_thread;
            uint32_t h_end = std::min(72u, (h_tid + 1) * hours_per_thread);
            if (h_start >= h_end) return;

            std::string& out_str = thread_output_strings[h_tid];
            out_str.reserve(1024 * 1024);
            char line_buf[256];

            for (uint32_t h = h_start; h < h_end; ++h) {
                const std::string& hour_str = HOURS[h];

                for (uint16_t rank = 0; rank < num_global_eps; ++rank) {
                    size_t c = (size_t)h * num_global_eps + rank;
                    uint32_t count = cell_totals[c];
                    if (count == 0) continue;

                    uint32_t off = cell_offsets[c];
                    uint32_t* slice = all_latencies.data() + off;

                    std::sort(slice, slice + count);

                    uint32_t p50 = nearest_rank_percentile_slice(slice, count, 50);
                    uint32_t p95 = nearest_rank_percentile_slice(slice, count, 95);
                    uint32_t p99 = nearest_rank_percentile_slice(slice, count, 99);
                    uint32_t errs = cell_errors[c];
                    uint64_t ppm = (static_cast<uint64_t>(errs) * 1000000ULL) / count;

                    std::string_view ep_sv = all_endpoints[sorted_order[rank]];

                    char* ptr = line_buf;
                    std::memcpy(ptr, hour_str.data(), hour_str.size());
                    ptr += hour_str.size();
                    *ptr++ = ' ';

                    std::memcpy(ptr, ep_sv.data(), ep_sv.size());
                    ptr += ep_sv.size();
                    *ptr++ = ' ';

                    ptr = write_u64(ptr, count);
                    *ptr++ = ' ';
                    ptr = write_u32(ptr, p50);
                    *ptr++ = ' ';
                    ptr = write_u32(ptr, p95);
                    *ptr++ = ' ';
                    ptr = write_u32(ptr, p99);
                    *ptr++ = ' ';
                    ptr = write_u64(ptr, errs);
                    *ptr++ = ' ';
                    ptr = write_u64(ptr, ppm);
                    *ptr++ = '\n';

                    out_str.append(line_buf, ptr - line_buf);
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) return 1;

    ferocious::OutBuf out(fp);
    for (unsigned int h_tid = 0; h_tid < num_threads; ++h_tid) {
        out.puts(thread_output_strings[h_tid].data(), thread_output_strings[h_tid].size());
    }
    out.flush();
    fclose(fp);

    return 0;
}
