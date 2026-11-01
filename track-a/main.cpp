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
    uint32_t day = (p[8] - '0') * 10 + (p[9] - '0');
    uint32_t hr  = (p[11] - '0') * 10 + (p[12] - '0');
    if (day >= 30) {
        return (day - 30) * 24 + hr - 18;
    } else {
        return 30 + (day - 1) * 24 + hr;
    }
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
    };
    std::vector<Entry> table;
    std::vector<std::string_view> list;

    LocalDict() : table(LOCAL_DICT_CAP) {
        list.reserve(2048);
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
};

#pragma pack(push, 1)
struct RawItem {
    uint8_t  hour_idx;
    uint16_t ep_id;
    uint8_t  is_error;
    uint32_t latency;
};
#pragma pack(pop)

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

inline uint32_t skip_method(const char* p) {
    char c = *p;
    if (c == 'G' || c == 'P' && p[1] == 'U') return 4; // GET, PUT
    if (c == 'P' && p[1] == 'O' || c == 'H') return 5; // POST, HEAD
    if (c == 'P' && p[1] == 'A') return 6; // PATCH
    if (c == 'D') return 7; // DELETE
    if (c == 'O') return 8; // OPTIONS
    while (*p != ' ') p++;
    return 0; // fallback
}

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

    size_t chunk_bytes = total_size / num_threads;
    std::vector<size_t> chunk_starts(num_threads);
    std::vector<size_t> chunk_ends(num_threads);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        size_t start = tid * chunk_bytes;
        size_t end = (tid == num_threads - 1) ? total_size : (tid + 1) * chunk_bytes;
        if (tid > 0) {
            while (start < total_size && data[start] != '\n') start++;
            if (start < total_size) start++;
        }
        if (tid < num_threads - 1) {
            while (end < total_size && data[end] != '\n') end++;
            if (end < total_size) end++;
        }
        chunk_starts[tid] = start;
        chunk_ends[tid] = end;
    }

    std::vector<LocalDict> thread_dicts(num_threads);
    std::vector<std::vector<RawItem>> thread_items(num_threads);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        size_t est_lines = (chunk_ends[tid] - chunk_starts[tid]) / 70;
        thread_items[tid].reserve(est_lines + 8192);
    }

    // 1. Parallel parse with fast method skip and fast 8-byte hash
    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* cur = data + chunk_starts[tid];
            const char* limit = data + chunk_ends[tid];
            auto& dict = thread_dicts[tid];
            auto& my_items = thread_items[tid];

            while (cur < limit) {
                const char* line_start = cur;
                const char* nl = static_cast<const char*>(std::memchr(cur, '\n', limit - cur));
                const char* line_end = nl ? nl : limit;
                cur = nl ? nl + 1 : limit;

                if (line_end - line_start < 30) continue;

                uint32_t hour_idx = parse_hour_idx(line_start);

                // Fast Skip Method
                const char* p = line_start + 28;
                uint32_t m_skip = skip_method(p);
                if (m_skip > 0) {
                    p += m_skip;
                } else {
                    while (p < line_end && *p != ' ') p++;
                    if (p < line_end) p++;
                }

                // Fast Endpoint Scan via memchr
                const char* ep_start = p;
                const char* sp = static_cast<const char*>(std::memchr(p, ' ', line_end - p));
                if (!sp) continue;
                uint32_t ep_len = (uint32_t)(sp - ep_start);
                p = sp + 1;

                uint16_t ep_id = dict.get_or_insert(ep_start, ep_len);

                // Status
                if (p + 3 > line_end) continue;
                uint8_t is_err = (p[0] == '5');
                p += 4;

                // Latency (fast digit parse)
                uint32_t latency = 0;
                while (p < line_end && *p >= '0' && *p <= '9') {
                    latency = latency * 10 + (*p - '0');
                    p++;
                }

                my_items.push_back({(uint8_t)hour_idx, ep_id, is_err, latency});
            }
        });
    }

    for (auto& w : workers) w.join();
    workers.clear();

    // 2. Build global unified endpoint dictionary
    std::vector<std::string_view> all_endpoints;
    all_endpoints.reserve(2048);

    static constexpr size_t GLOBAL_MASK = 4095;
    struct GlobalSlot {
        uint32_t hash = 0;
        std::string_view sv;
        uint16_t id = 0;
    };
    std::vector<GlobalSlot> global_slots(4096);

    auto get_or_insert_global = [&](std::string_view sv) -> uint16_t {
        uint64_t h64 = hash_bytes_fast(sv.data(), sv.size());
        uint32_t h = (uint32_t)(h64 ^ (h64 >> 32));
        if (h == 0) h = 1;
        size_t idx = h & GLOBAL_MASK;
        while (true) {
            auto& s = global_slots[idx];
            if (s.hash == 0) {
                uint16_t id = (uint16_t)all_endpoints.size();
                all_endpoints.push_back(sv);
                s.hash = h;
                s.sv = sv;
                s.id = id;
                return id;
            }
            if (s.hash == h && s.sv == sv) {
                return s.id;
            }
            idx = (idx + 1) & GLOBAL_MASK;
        }
    };

    std::vector<std::vector<uint16_t>> remap(num_threads);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        remap[tid].resize(thread_dicts[tid].list.size());
        for (size_t i = 0; i < thread_dicts[tid].list.size(); ++i) {
            remap[tid][i] = get_or_insert_global(thread_dicts[tid].list[i]);
        }
    }

    uint32_t num_global_eps = (uint32_t)all_endpoints.size();

    // 3. Sort endpoints lexicographically by exact unsigned byte values
    std::vector<uint16_t> sorted_order(num_global_eps);
    for (uint16_t i = 0; i < num_global_eps; ++i) sorted_order[i] = i;
    std::sort(sorted_order.begin(), sorted_order.end(), [&](uint16_t a, uint16_t b) {
        return all_endpoints[a] < all_endpoints[b];
    });

    std::vector<uint16_t> global_to_rank(num_global_eps);
    for (uint16_t rank = 0; rank < num_global_eps; ++rank) {
        global_to_rank[sorted_order[rank]] = rank;
    }

    // Precompute combined remap lookup table: local_ep_id -> rank for each thread
    std::vector<std::vector<uint16_t>> local_to_rank(num_threads);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        local_to_rank[tid].resize(thread_dicts[tid].list.size());
        for (size_t i = 0; i < thread_dicts[tid].list.size(); ++i) {
            local_to_rank[tid][i] = global_to_rank[remap[tid][i]];
        }
    }

    // 4. Counting Sort into Contiguous 12.4 MB Latency Array
    size_t num_cells = 72 * (size_t)num_global_eps;

    // Per-thread cell counts: thread_cell_counts[tid][cell_idx]
    std::vector<std::vector<uint32_t>> thread_cell_counts(num_threads, std::vector<uint32_t>(num_cells, 0));
    std::vector<std::vector<uint32_t>> thread_cell_errors(num_threads, std::vector<uint32_t>(num_cells, 0));

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            auto& counts = thread_cell_counts[tid];
            auto& errors = thread_cell_errors[tid];
            const auto& l2r = local_to_rank[tid];
            for (auto& item : thread_items[tid]) {
                uint32_t rank = l2r[item.ep_id];
                item.ep_id = (uint16_t)rank; // Cache rank directly in item!
                uint32_t c = (uint32_t)item.hour_idx * num_global_eps + rank;
                counts[c]++;
                errors[c] += item.is_error;
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // Global cell offsets and totals
    std::vector<uint32_t> cell_offsets(num_cells + 1, 0);
    std::vector<uint32_t> cell_totals(num_cells, 0);
    std::vector<uint32_t> cell_errors(num_cells, 0);

    // Compute cell totals
    for (size_t c = 0; c < num_cells; ++c) {
        uint32_t tot = 0;
        uint32_t err = 0;
        for (unsigned int tid = 0; tid < num_threads; ++tid) {
            tot += thread_cell_counts[tid][c];
            err += thread_cell_errors[tid][c];
        }
        cell_totals[c] = tot;
        cell_errors[c] = err;
    }

    // Prefix sums to find exact start offset of each cell in all_latencies
    uint32_t current_offset = 0;
    for (size_t c = 0; c < num_cells; ++c) {
        cell_offsets[c] = current_offset;
        current_offset += cell_totals[c];
    }
    cell_offsets[num_cells] = current_offset;
    size_t total_items = current_offset;

    // The Flat Contiguous 12.4 MB Latency Array (Resides in L3 Cache)
    std::vector<uint32_t> all_latencies(total_items);

    // Compute thread write offsets
    std::vector<std::vector<uint32_t>> thread_write_offsets(num_threads, std::vector<uint32_t>(num_cells, 0));
    for (size_t c = 0; c < num_cells; ++c) {
        uint32_t base = cell_offsets[c];
        for (unsigned int tid = 0; tid < num_threads; ++tid) {
            thread_write_offsets[tid][c] = base;
            base += thread_cell_counts[tid][c];
        }
    }

    // Scatter latencies directly into flat array (using cached rank!)
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            auto& cur_offs = thread_write_offsets[tid];
            for (const auto& item : thread_items[tid]) {
                uint32_t c = (uint32_t)item.hour_idx * num_global_eps + item.ep_id;
                uint32_t dest = cur_offs[c]++;
                all_latencies[dest] = item.latency;
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // Release thread raw items to free memory immediately
    for (auto& ti : thread_items) std::vector<RawItem>().swap(ti);

    // 5. Parallel Format & Percentiles by Hour
    std::vector<std::string> thread_output_strings(num_threads);
    uint32_t hours_per_thread = (72 + num_threads - 1) / num_threads;

    for (unsigned int h_tid = 0; h_tid < num_threads; ++h_tid) {
        workers.emplace_back([&, h_tid]() {
            uint32_t h_start = h_tid * hours_per_thread;
            uint32_t h_end = std::min(72u, h_start + hours_per_thread);
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

                    // Sort slice in-place directly in contiguous L3-resident memory
                    std::sort(slice, slice + count);

                    uint32_t p50 = nearest_rank_percentile_slice(slice, count, 50);
                    uint32_t p95 = nearest_rank_percentile_slice(slice, count, 95);
                    uint32_t p99 = nearest_rank_percentile_slice(slice, count, 99);
                    uint32_t errs = cell_errors[c];
                    uint64_t ppm = (static_cast<uint64_t>(errs) * 1000000ULL) / count;

                    std::string_view ep_sv = all_endpoints[sorted_order[rank]];

                    // Fast single-pass line formatting in L1 cache
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
                    ptr = write_u32(ptr, errs);
                    *ptr++ = ' ';
                    ptr = write_u64(ptr, ppm);
                    *ptr++ = '\n';

                    out_str.append(line_buf, ptr - line_buf);
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    // 6. Emit output file
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
