// FERROCIOUS Track A — 12-Thread Lock-Free Parallel Ingest Engine (specs/a-ingest.md)
// Zero External Dependencies (D = 1.00). C++20.

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>
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

inline uint64_t hash_bytes(const char* p, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)p[i];
        h *= 0x100000001b3ULL;
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
        uint64_t h64 = hash_bytes(p, len);
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
struct EpItem {
    uint16_t ep_id;
    uint32_t latency;
    uint8_t is_error;
};
#pragma pack(pop)

inline uint32_t nearest_rank_percentile(const std::vector<uint32_t>& sorted_vals, uint32_t p) {
    size_t n = sorted_vals.size();
    size_t rank = (p * n + 99) / 100;
    return sorted_vals[rank - 1];
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
    std::vector<std::vector<std::vector<EpItem>>> thread_hour_items(num_threads);
    for (auto& thi : thread_hour_items) {
        thi.resize(72);
        for (auto& h_vec : thi) h_vec.reserve(4500);
    }

    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* cur = data + chunk_starts[tid];
            const char* limit = data + chunk_ends[tid];
            auto& dict = thread_dicts[tid];
            auto& my_hour_items = thread_hour_items[tid];

            while (cur < limit) {
                const char* line_start = cur;
                const char* nl = static_cast<const char*>(std::memchr(cur, '\n', limit - cur));
                const char* line_end = nl ? nl : limit;
                cur = nl ? nl + 1 : limit;

                if (line_end - line_start < 30) continue;

                uint32_t hour_idx = parse_hour_idx(line_start);

                // Skip method
                const char* p = line_start + 28;
                while (p < line_end && *p != ' ') p++;
                if (p >= line_end) continue;
                p++;

                // Endpoint
                const char* ep_start = p;
                while (p < line_end && *p != ' ') p++;
                if (p >= line_end) continue;
                uint32_t ep_len = (uint32_t)(p - ep_start);
                p++;

                uint16_t ep_id = dict.get_or_insert(ep_start, ep_len);

                // Status
                if (p + 3 > line_end) continue;
                uint8_t is_err = (p[0] == '5');
                p += 4;

                // Latency
                uint32_t latency = 0;
                while (p < line_end && *p >= '0' && *p <= '9') {
                    latency = latency * 10 + (*p - '0');
                    p++;
                }

                my_hour_items[hour_idx].push_back({ep_id, latency, is_err});
            }
        });
    }

    for (auto& w : workers) w.join();

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
        uint64_t h64 = hash_bytes(sv.data(), sv.size());
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

    // 4. Parallel Hour-Bucketed Grouping & Formatting across hardware threads
    std::vector<std::string> thread_output_strings(num_threads);
    uint32_t hours_per_thread = (72 + num_threads - 1) / num_threads;

    struct Group {
        std::vector<uint32_t> latencies;
        uint32_t errors = 0;
    };

    std::vector<std::thread> format_workers;
    for (unsigned int h_tid = 0; h_tid < num_threads; ++h_tid) {
        format_workers.emplace_back([&, h_tid]() {
            uint32_t h_start = h_tid * hours_per_thread;
            uint32_t h_end = std::min(72u, h_start + hours_per_thread);
            if (h_start >= h_end) return;

            std::vector<Group> local_groups(num_global_eps);

            std::string& out_str = thread_output_strings[h_tid];
            out_str.reserve(1024 * 1024);

            char buf[64];

            for (uint32_t h = h_start; h < h_end; ++h) {
                for (auto& g : local_groups) {
                    g.latencies.clear();
                    g.errors = 0;
                }

                for (unsigned int tid = 0; tid < num_threads; ++tid) {
                    const auto& r = remap[tid];
                    for (const auto& item : thread_hour_items[tid][h]) {
                        uint16_t rank = global_to_rank[r[item.ep_id]];
                        local_groups[rank].latencies.push_back(item.latency);
                        local_groups[rank].errors += item.is_error;
                    }
                }

                const std::string& hour_str = HOURS[h];

                for (uint16_t rank = 0; rank < num_global_eps; ++rank) {
                    auto& g = local_groups[rank];
                    if (g.latencies.empty()) continue;

                    std::sort(g.latencies.begin(), g.latencies.end());
                    size_t count = g.latencies.size();
                    uint32_t p50 = nearest_rank_percentile(g.latencies, 50);
                    uint32_t p95 = nearest_rank_percentile(g.latencies, 95);
                    uint32_t p99 = nearest_rank_percentile(g.latencies, 99);
                    uint64_t ppm = (static_cast<uint64_t>(g.errors) * 1000000ULL) / count;

                    std::string_view ep_sv = all_endpoints[sorted_order[rank]];

                    out_str.append(hour_str);
                    out_str.push_back(' ');
                    out_str.append(ep_sv);
                    out_str.push_back(' ');

                    int len = snprintf(buf, sizeof(buf), "%zu %u %u %u %u %llu\n",
                                       count, p50, p95, p99, g.errors, (unsigned long long)ppm);
                    out_str.append(buf, len);
                }
            }
        });
    }

    for (auto& w : format_workers) w.join();

    // 5. Emit output file
    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) return 1;

    for (unsigned int h_tid = 0; h_tid < num_threads; ++h_tid) {
        fwrite(thread_output_strings[h_tid].data(), 1, thread_output_strings[h_tid].size(), fp);
    }
    fclose(fp);
    return 0;
}
