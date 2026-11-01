// FERROCIOUS — Track A: Bare-Metal Ingest Engine
// Conforms strictly to Hackathon Raptors Track A Specification (specs/a-ingest.md)
// Zero External Dependencies (D = 1.00)

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <map>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>
#include <atomic>
#include "ferocious_common.hpp"

struct GroupData {
    std::vector<uint32_t> latencies;
    uint32_t errors = 0;
};

inline uint32_t nearest_rank_percentile(const std::vector<uint32_t>& sorted_vals, uint32_t p) {
    size_t n = sorted_vals.size();
    size_t rank = (p * n + 99) / 100;
    return sorted_vals[rank - 1];
}

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    std::string input_path = data_dir + "/requests.log";
    std::string output_path = out_dir + "/summary.txt";

    ferocious::MappedFile mf(input_path.c_str());
    if (!mf.ok) {
        std::cerr << "Failed to open input: " << input_path << "\n";
        return 1;
    }

    const char* data = static_cast<const char*>(mf.data);
    size_t total_size = mf.size;

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    struct LocalGroup {
        std::vector<uint32_t> latencies;
        uint32_t errors = 0;
    };

    // Thread-local collections
    using KeyType = std::pair<std::string, std::string>;
    std::vector<std::map<KeyType, LocalGroup>> thread_maps(num_threads);

    size_t chunk_bytes = total_size / num_threads;
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            size_t start = tid * chunk_bytes;
            size_t end = (tid == num_threads - 1) ? total_size : (tid + 1) * chunk_bytes;

            // Align start to the beginning of a line
            if (tid > 0) {
                while (start < total_size && data[start] != '\n') {
                    start++;
                }
                if (start < total_size) start++; // skip '\n'
            }

            // Align end to the end of a line
            if (tid < num_threads - 1) {
                while (end < total_size && data[end] != '\n') {
                    end++;
                }
                if (end < total_size) end++; // include '\n'
            }

            auto& local_map = thread_maps[tid];
            const char* cur = data + start;
            const char* limit = data + end;

            while (cur < limit) {
                const char* line_start = cur;
                const char* line_end = static_cast<const char*>(std::memchr(cur, '\n', limit - cur));
                if (!line_end) {
                    line_end = limit;
                    cur = limit;
                } else {
                    cur = line_end + 1;
                }

                if (line_end == line_start) continue;

                // Grammar: ts SP method SP endpoint SP status SP latency
                // ts is exactly 27 bytes: YYYY-MM-DDTHH:MM:SS.ffffffZ
                // Hour is first 13 chars + ":00:00Z"
                if (line_start + 27 > line_end || line_start[26] != 'Z') continue;

                std::string hour(line_start, 13);
                hour += ":00:00Z";

                const char* p = line_start + 28; // skip ts and space

                // Skip method
                while (p < line_end && *p != ' ') p++;
                if (p >= line_end) continue;
                p++; // skip space

                // Endpoint
                const char* ep_start = p;
                while (p < line_end && *p != ' ') p++;
                if (p >= line_end) continue;
                std::string endpoint(ep_start, p - ep_start);
                p++; // skip space

                // Status (3 digits)
                if (p + 3 > line_end) continue;
                int status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
                p += 3;
                if (p >= line_end || *p != ' ') continue;
                p++; // skip space

                // Latency (uint32)
                uint32_t latency = 0;
                while (p < line_end && *p >= '0' && *p <= '9') {
                    latency = latency * 10 + (*p - '0');
                    p++;
                }

                auto& grp = local_map[{hour, endpoint}];
                grp.latencies.push_back(latency);
                if (status >= 500 && status <= 599) {
                    grp.errors++;
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    // Merge all thread-local maps into a sorted global map
    std::map<KeyType, GroupData> global_map;
    for (auto& lmap : thread_maps) {
        for (auto& [key, lgrp] : lmap) {
            auto& ggrp = global_map[key];
            ggrp.latencies.insert(ggrp.latencies.end(), lgrp.latencies.begin(), lgrp.latencies.end());
            ggrp.errors += lgrp.errors;
        }
    }

    // Ensure output directory exists
    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) {
        std::cerr << "Failed to open output: " << output_path << "\n";
        return 1;
    }

    ferocious::OutBuf out(fp);

    for (auto& [key, grp] : global_map) {
        std::sort(grp.latencies.begin(), grp.latencies.end());
        size_t count = grp.latencies.size();
        uint32_t p50 = nearest_rank_percentile(grp.latencies, 50);
        uint32_t p95 = nearest_rank_percentile(grp.latencies, 95);
        uint32_t p99 = nearest_rank_percentile(grp.latencies, 99);
        uint64_t ppm = (static_cast<uint64_t>(grp.errors) * 1000000ULL) / count;

        out.puts(key.first.data(), key.first.size());
        out.put(' ');
        out.puts(key.second.data(), key.second.size());
        out.put(' ');
        out.u64(count);
        out.put(' ');
        out.u64(p50);
        out.put(' ');
        out.u64(p95);
        out.put(' ');
        out.u64(p99);
        out.put(' ');
        out.u64(grp.errors);
        out.put(' ');
        out.u64(ppm);
        out.put('\n');
    }

    out.flush();
    fclose(fp);
    return 0;
}
