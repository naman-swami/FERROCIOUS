// FERROCIOUS Track A — 1-Pass Counting Sort Engine (specs/a-ingest.md)
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

inline uint64_t fnv1a(const char* p, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)p[i];
        h *= 0x100000001b3ULL;
    }
    return h == 0 ? 1 : h;
}

static constexpr size_t MAP_CAP = 4096;
static constexpr size_t MAP_MASK = MAP_CAP - 1;

struct ThreadStrMap {
    uint64_t keys[MAP_CAP]{};
    uint16_t ids[MAP_CAP]{};
};

struct GlobalHashToId {
    uint64_t keys[MAP_CAP]{};
    uint16_t ids[MAP_CAP]{};

    inline void insert(uint64_t hash, uint16_t id) {
        size_t idx = hash & MAP_MASK;
        while (keys[idx] != 0 && keys[idx] != hash) idx = (idx + 1) & MAP_MASK;
        keys[idx] = hash;
        ids[idx] = id;
    }

    inline uint16_t lookup(uint64_t hash) const {
        size_t idx = hash & MAP_MASK;
        while (keys[idx] != hash) idx = (idx + 1) & MAP_MASK;
        return ids[idx];
    }
};

#pragma pack(push, 1)
struct RawItem {
    uint8_t  hour;
    uint8_t  is_err;
    uint16_t local_ep_id;
    uint32_t latency;
};
#pragma pack(pop)

inline char* write_u32(char* p, uint32_t v) {
    char buf[12];
    char* q = buf + sizeof(buf);
    if (v == 0) {
        *p++ = '0';
        return p;
    }
    while (v > 0) {
        *--q = '0' + (v % 10);
        v /= 10;
    }
    size_t len = buf + sizeof(buf) - q;
    std::memcpy(p, q, len);
    return p + len;
}

inline char* write_u64(char* p, uint64_t v) {
    char buf[24];
    char* q = buf + sizeof(buf);
    if (v == 0) {
        *p++ = '0';
        return p;
    }
    while (v > 0) {
        *--q = '0' + (v % 10);
        v /= 10;
    }
    size_t len = buf + sizeof(buf) - q;
    std::memcpy(p, q, len);
    return p + len;
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

    std::vector<std::vector<RawItem>> thread_raw_items(num_threads);
    std::vector<std::vector<std::pair<uint64_t, std::string_view>>> thread_unique_strings(num_threads);
    std::vector<ThreadStrMap> thread_maps(num_threads);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        size_t est_lines = (chunk_ends[tid] - chunk_starts[tid]) / 60;
        thread_raw_items[tid].reserve(est_lines + 8192);
        thread_unique_strings[tid].reserve(2048);
    }

    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* cur = data + chunk_starts[tid];
            const char* limit = data + chunk_ends[tid];
            auto& items = thread_raw_items[tid];
            auto& ustrings = thread_unique_strings[tid];
            auto& smap = thread_maps[tid];

            while (cur < limit) {
                const char* line_start = cur;
                const char* nl = static_cast<const char*>(std::memchr(cur, '\n', limit - cur));
                const char* line_end = nl ? nl : limit;
                cur = nl ? nl + 1 : limit;

                if (line_end - line_start < 30) continue;

                uint32_t hour_idx = parse_hour_idx(line_start);

                // Method
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

                uint64_t h = fnv1a(ep_start, ep_len);
                size_t slot = h & MAP_MASK;
                while (smap.keys[slot] != 0 && smap.keys[slot] != h) slot = (slot + 1) & MAP_MASK;

                uint16_t local_ep_id;
                if (smap.keys[slot] == 0) {
                    local_ep_id = (uint16_t)ustrings.size();
                    ustrings.emplace_back(h, std::string_view(ep_start, ep_len));
                    smap.keys[slot] = h;
                    smap.ids[slot] = local_ep_id;
                } else {
                    local_ep_id = smap.ids[slot];
                }

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

                items.push_back({(uint8_t)hour_idx, is_err, local_ep_id, latency});
            }
        });
    }

    for (auto& w : workers) w.join();
    workers.clear();

    // 2. Global deduplication and lexicographic sorting of endpoints
    std::vector<std::pair<std::string_view, uint64_t>> all_endpoints_pairs;
    all_endpoints_pairs.reserve(4096);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        for (const auto& p : thread_unique_strings[tid]) {
            all_endpoints_pairs.emplace_back(p.second, p.first);
        }
    }

    std::sort(all_endpoints_pairs.begin(), all_endpoints_pairs.end(),
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });

    std::vector<std::string_view> global_endpoints;
    global_endpoints.reserve(2048);
    GlobalHashToId global_hash_map;

    for (const auto& p : all_endpoints_pairs) {
        if (global_endpoints.empty() || global_endpoints.back() != p.first) {
            uint16_t gid = (uint16_t)global_endpoints.size();
            global_endpoints.push_back(p.first);
            global_hash_map.insert(p.second, gid);
        }
    }

    uint32_t num_eps = (uint32_t)global_endpoints.size();
    uint32_t total_buckets = 72 * num_eps;

    // Build thread remap tables
    std::vector<std::vector<uint16_t>> thread_remap(num_threads);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        thread_remap[tid].resize(thread_unique_strings[tid].size());
        for (size_t i = 0; i < thread_unique_strings[tid].size(); ++i) {
            thread_remap[tid][i] = global_hash_map.lookup(thread_unique_strings[tid][i].first);
        }
    }

    // 3. Parallel 1-Pass Counting Sort: Compute Bucket Counts & Errors per thread
    std::vector<std::vector<uint32_t>> thread_bucket_counts(num_threads);
    std::vector<std::vector<uint32_t>> thread_bucket_errors(num_threads);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        thread_bucket_counts[tid].assign(total_buckets, 0);
        thread_bucket_errors[tid].assign(total_buckets, 0);
    }

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const auto& items = thread_raw_items[tid];
            const auto& remap = thread_remap[tid];
            auto& counts = thread_bucket_counts[tid];
            auto& errors = thread_bucket_errors[tid];
            for (const auto& item : items) {
                uint16_t gid = remap[item.local_ep_id];
                uint32_t b = (uint32_t)item.hour * num_eps + gid;
                counts[b]++;
                errors[b] += item.is_err;
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // 4. Prefix Sums & Global Scatter Offsets & Aggregated Bucket Errors
    std::vector<std::vector<uint32_t>> thread_scatter_offsets(num_threads);
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        thread_scatter_offsets[tid].resize(total_buckets);
    }

    std::vector<uint32_t> bucket_starts(total_buckets + 1, 0);
    std::vector<uint32_t> bucket_errors(total_buckets, 0);
    uint32_t running_total = 0;
    for (uint32_t b = 0; b < total_buckets; ++b) {
        bucket_starts[b] = running_total;
        uint32_t b_err = 0;
        for (unsigned int tid = 0; tid < num_threads; ++tid) {
            thread_scatter_offsets[tid][b] = running_total;
            running_total += thread_bucket_counts[tid][b];
            b_err += thread_bucket_errors[tid][b];
        }
        bucket_errors[b] = b_err;
    }
    bucket_starts[total_buckets] = running_total;
    size_t total_items = running_total;

    // Free thread bucket error buffers
    thread_bucket_errors.clear();
    thread_bucket_errors.shrink_to_fit();

    // 5. Parallel Scatter of Latencies ONLY
    std::vector<uint32_t> global_latencies(total_items);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const auto& items = thread_raw_items[tid];
            const auto& remap = thread_remap[tid];
            auto& offsets = thread_scatter_offsets[tid];
            for (const auto& item : items) {
                uint16_t gid = remap[item.local_ep_id];
                uint32_t b = (uint32_t)item.hour * num_eps + gid;
                uint32_t dst = offsets[b]++;
                global_latencies[dst] = item.latency;
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // Free raw items memory
    thread_raw_items.clear();
    thread_raw_items.shrink_to_fit();

    // 6. Parallel Formatting per Hour Chunk
    std::vector<std::string> thread_outputs(num_threads);
    uint32_t hours_per_thread = (72 + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            uint32_t h_start = tid * hours_per_thread;
            uint32_t h_end = std::min(72u, h_start + hours_per_thread);
            if (h_start >= h_end) return;

            std::string& out = thread_outputs[tid];
            out.resize(2 * 1024 * 1024);
            char* out_ptr = out.data();

            for (uint32_t h = h_start; h < h_end; ++h) {
                const std::string& hour_str = HOURS[h];
                for (uint32_t gid = 0; gid < num_eps; ++gid) {
                    uint32_t b = h * num_eps + gid;
                    uint32_t start_idx = bucket_starts[b];
                    uint32_t end_idx = bucket_starts[b + 1];
                    uint32_t count = end_idx - start_idx;
                    if (count == 0) continue;

                    uint32_t* lats = &global_latencies[start_idx];
                    std::sort(lats, lats + count);

                    size_t rank50 = (50ULL * count + 99) / 100;
                    size_t rank95 = (95ULL * count + 99) / 100;
                    size_t rank99 = (99ULL * count + 99) / 100;

                    uint32_t p50 = lats[rank50 - 1];
                    uint32_t p95 = lats[rank95 - 1];
                    uint32_t p99 = lats[rank99 - 1];

                    uint32_t errors = bucket_errors[b];
                    uint64_t ppm = (static_cast<uint64_t>(errors) * 1000000ULL) / count;

                    std::string_view ep = global_endpoints[gid];

                    std::memcpy(out_ptr, hour_str.data(), hour_str.size());
                    out_ptr += hour_str.size();
                    *out_ptr++ = ' ';

                    std::memcpy(out_ptr, ep.data(), ep.size());
                    out_ptr += ep.size();
                    *out_ptr++ = ' ';

                    out_ptr = write_u32(out_ptr, count);
                    *out_ptr++ = ' ';
                    out_ptr = write_u32(out_ptr, p50);
                    *out_ptr++ = ' ';
                    out_ptr = write_u32(out_ptr, p95);
                    *out_ptr++ = ' ';
                    out_ptr = write_u32(out_ptr, p99);
                    *out_ptr++ = ' ';
                    out_ptr = write_u32(out_ptr, errors);
                    *out_ptr++ = ' ';
                    out_ptr = write_u64(out_ptr, ppm);
                    *out_ptr++ = '\n';
                }
            }
            out.resize(out_ptr - out.data());
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // 7. Write output
    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) return 1;

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        fwrite(thread_outputs[tid].data(), 1, thread_outputs[tid].size(), fp);
    }
    fclose(fp);

    return 0;
}
