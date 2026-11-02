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
#include <immintrin.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sched.h>
#endif

namespace {

constexpr size_t MAX_ENDPOINTS = 2048;
constexpr size_t NUM_HOURS = 72;
constexpr size_t NUM_CELLS = NUM_HOURS * MAX_ENDPOINTS;

inline void fmt_hour(uint32_t h, char* out) {
    uint32_t day = 1 + h / 24;
    uint32_t hr  = h % 24;
    out[0] = '2'; out[1] = '0'; out[2] = '2'; out[3] = '6'; out[4] = '-';
    out[5] = '0'; out[6] = '3'; out[7] = '-';
    out[8] = '0' + (day / 10); out[9] = '0' + (day % 10);
    out[10] = 'T';
    out[11] = '0' + (hr / 10);  out[12] = '0' + (hr % 10);
    out[13] = ':'; out[14] = '0'; out[15] = '0'; out[16] = ':';
    out[17] = '0'; out[18] = '0'; out[19] = 'Z';
}

inline uint32_t parse_hour_idx(const char* p) {
    uint32_t day = (p[8] - '0') * 10 + (p[9] - '0');
    uint32_t hour = (p[11] - '0') * 10 + (p[12] - '0');
    return (day - 1) * 24 + hour;
}

inline uint8_t get_method_len(const char* p) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    if ((w & 0xFFFFFF) == 0x205445) return 4;
    if ((w & 0xFFFFFFFF) == 0x2054534F) return 5;
    if ((w & 0xFFFFFFFFFFULL) == 0x2048435441) return 6;
    if ((w & 0xFFFFFFFFFFULL) == 0x2054454C45) return 7;
    return 4;
}

struct DictSlot {
    uint32_t hash;
    uint16_t len;
    uint32_t offset;
    uint64_t prefix;
    std::atomic<uint16_t> id{0};
};

constexpr size_t DICT_CAPACITY = 8192;
constexpr size_t DICT_MASK = DICT_CAPACITY - 1;

alignas(64) DictSlot g_dict[DICT_CAPACITY];
alignas(64) char g_str_pool[128 * 1024];
std::atomic<uint32_t> g_str_pool_len{0};
std::atomic<uint16_t> g_num_endpoints{0};

std::string g_endpoint_list[MAX_ENDPOINTS];
uint16_t g_old_to_lex[MAX_ENDPOINTS];

inline uint32_t hash_endpoint(const char* p, uint16_t len, uint64_t prefix) {
    uint64_t h = prefix ^ (uint64_t)len * 0x517cc1b727220a95ULL;
    if (len > 8) {
        uint64_t tail = 0;
        std::memcpy(&tail, p + len - 8, 8);
        h ^= tail * 0x9e3779b97f4a7c15ULL;
    }
    return (uint32_t)(h ^ (h >> 32));
}

inline uint16_t dict_get_or_insert(const char* p, uint16_t len) {
    uint64_t prefix = 0;
    if (len >= 8) {
        std::memcpy(&prefix, p, 8);
    } else {
        std::memcpy(&prefix, p, len);
    }
    uint32_t h = hash_endpoint(p, len, prefix);
    size_t idx = h & DICT_MASK;

    while (true) {
        auto& slot = g_dict[idx];
        uint16_t sid = slot.id.load(std::memory_order_acquire);
        if (sid == 0) {
            uint16_t expected = 0;
            if (slot.id.compare_exchange_strong(expected, 0xFFFF, std::memory_order_acq_rel)) {
                uint32_t off = g_str_pool_len.fetch_add(len, std::memory_order_relaxed);
                std::memcpy(g_str_pool + off, p, len);
                slot.hash = h;
                slot.len = len;
                slot.offset = off;
                slot.prefix = prefix;
                uint16_t new_id = g_num_endpoints.fetch_add(1, std::memory_order_relaxed);
                g_endpoint_list[new_id] = std::string(p, len);
                slot.id.store(new_id + 1, std::memory_order_release);
                return new_id;
            }
            sid = slot.id.load(std::memory_order_acquire);
        }

        while (sid == 0xFFFF) {
            _mm_pause();
            sid = slot.id.load(std::memory_order_acquire);
        }

        if (slot.hash == h && slot.len == len && slot.prefix == prefix) {
            if (len <= 8 || std::memcmp(g_str_pool + slot.offset + 8, p + 8, len - 8) == 0) {
                return sid - 1;
            }
        }
        idx = (idx + 1) & DICT_MASK;
    }
}

inline uint16_t dict_find_lex(const char* p, uint16_t len) {
    uint64_t prefix = 0;
    if (len >= 8) {
        std::memcpy(&prefix, p, 8);
    } else {
        std::memcpy(&prefix, p, len);
    }
    uint32_t h = hash_endpoint(p, len, prefix);
    size_t idx = h & DICT_MASK;
    while (true) {
        const auto& slot = g_dict[idx];
        if (slot.hash == h && slot.len == len && slot.prefix == prefix) {
            if (len <= 8 || std::memcmp(g_str_pool + slot.offset + 8, p + 8, len - 8) == 0) {
                return slot.id.load(std::memory_order_relaxed) - 1;
            }
        }
        idx = (idx + 1) & DICT_MASK;
    }
}

inline void insertion_sort(uint32_t* a, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        uint32_t key = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1] > key) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = key;
    }
}

template <typename T>
inline char* write_uint(char* p, T val) {
    char buf[32];
    char* cur = buf + 32;
    if (val == 0) { *p++ = '0'; return p; }
    while (val > 0) { *--cur = '0' + (val % 10); val /= 10; }
    size_t len = (buf + 32) - cur;
    std::memcpy(p, cur, len);
    return p + len;
}

struct ThreadChunk {
    size_t start;
    size_t len;
};

#ifdef _WIN32
inline ssize_t my_pread(HANDLE hFile, void* buf, size_t count, size_t offset) {
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf, (DWORD)count, &bytesRead, &ov)) return -1;
    return bytesRead;
}
#else
inline ssize_t my_pread(int fd, void* buf, size_t count, size_t offset) {
    return pread(fd, buf, count, offset);
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "data/official_full/a";
    std::string out_dir  = (argc > 2) ? argv[2] : "out_test_a";

    std::string input_path = data_dir + "/requests.log";
    std::string output_path = out_dir + "/summary.txt";

#ifdef _WIN32
    HANDLE hFile = CreateFileA(input_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 1;
    LARGE_INTEGER liSize;
    GetFileSizeEx(hFile, &liSize);
    size_t total_size = liSize.QuadPart;
#else
    int fd = open(input_path.c_str(), O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    fstat(fd, &st);
    size_t total_size = st.st_size;
#endif

#ifdef _WIN32
    unsigned int num_threads = std::thread::hardware_concurrency();
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    sched_getaffinity(0, sizeof(cpuset), &cpuset);
    unsigned int num_threads = CPU_COUNT(&cpuset);
#endif
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    constexpr size_t CHUNK_SIZE = 256 * 1024;
    size_t num_chunks = (total_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    std::atomic<uint32_t> next_chunk{0};

    std::vector<std::vector<ThreadChunk>> thread_chunks(num_threads);
    std::vector<std::vector<uint32_t>> thread_counts(num_threads, std::vector<uint32_t>(NUM_CELLS, 0));
    std::vector<std::vector<uint32_t>> thread_errors(num_threads, std::vector<uint32_t>(NUM_CELLS, 0));

    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            auto& my_chunks = thread_chunks[tid];
            auto& counts = thread_counts[tid];
            auto& errors = thread_errors[tid];
            alignas(64) char chunk_buf[CHUNK_SIZE + 4096];

            while (true) {
                uint32_t c = next_chunk.fetch_add(1, std::memory_order_relaxed);
                if (c >= num_chunks) break;

                size_t offset = (size_t)c * CHUNK_SIZE;
                size_t to_read = std::min(CHUNK_SIZE + 4096, total_size - offset);

#ifdef _WIN32
                ssize_t n = my_pread(hFile, chunk_buf, to_read, offset);
#else
                ssize_t n = my_pread(fd, chunk_buf, to_read, offset);
#endif
                if (n <= 0) break;

                const char* cur = chunk_buf;
                const char* limit = chunk_buf + n;

                if (c > 0) {
                    while (cur < limit && *cur != '\n') cur++;
                    if (cur < limit) cur++;
                }

                const char* end = cur;
                if (c + 1 < num_chunks) {
                    const char* target_end = chunk_buf + std::min((size_t)n, CHUNK_SIZE);
                    while (target_end < limit && *target_end != '\n') target_end++;
                    if (target_end < limit) target_end++;
                    end = target_end;
                } else {
                    end = limit;
                }

                size_t file_start = offset + (cur - chunk_buf);
                size_t file_len = end - cur;
                my_chunks.push_back({file_start, file_len});

                const char* chunk_limit = end;
                while (cur + 38 < chunk_limit) {
                    const char* line_start = cur;
                    const char* nl = static_cast<const char*>(std::memchr(cur + 38, '\n', chunk_limit - (cur + 38)));
                    if (!nl) break;
                    cur = nl + 1;

                    const char* q = nl - 1;
                    while (*q != ' ') q--;

                    bool is_err = (q[-3] >= '5');
                    const char* ep_end = q - 4;

                    uint8_t mlen = get_method_len(line_start + 28);
                    const char* ep_start = line_start + 28 + mlen;
                    uint16_t ep_len = (uint16_t)(ep_end - ep_start);

                    uint32_t hour_idx = parse_hour_idx(line_start);
                    uint16_t ep_id = dict_get_or_insert(ep_start, ep_len);

                    size_t cell = (size_t)hour_idx * MAX_ENDPOINTS + ep_id;
                    counts[cell]++;
                    if (is_err) errors[cell]++;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    uint16_t K = g_num_endpoints.load();
    std::vector<uint16_t> sorted_order(K);
    for (uint16_t i = 0; i < K; ++i) sorted_order[i] = i;

    std::sort(sorted_order.begin(), sorted_order.end(), [&](uint16_t a, uint16_t b) {
        return g_endpoint_list[a] < g_endpoint_list[b];
    });

    for (uint16_t rank = 0; rank < K; ++rank) {
        g_old_to_lex[sorted_order[rank]] = rank;
    }

    for (size_t i = 0; i < DICT_CAPACITY; ++i) {
        if (g_dict[i].id.load() > 0 && g_dict[i].id.load() <= K) {
            uint16_t old_id = g_dict[i].id.load() - 1;
            g_dict[i].id.store(g_old_to_lex[old_id] + 1, std::memory_order_relaxed);
        }
    }

    size_t active_cells = NUM_HOURS * K;
    std::vector<uint32_t> g_counts(active_cells, 0);
    std::vector<uint32_t> g_errors(active_cells, 0);

    std::vector<std::vector<uint32_t>> thread_write_ptrs(num_threads, std::vector<uint32_t>(active_cells, 0));

    for (size_t h = 0; h < NUM_HOURS; ++h) {
        for (uint16_t rank = 0; rank < K; ++rank) {
            uint16_t old_id = sorted_order[rank];
            size_t src_idx = h * MAX_ENDPOINTS + old_id;
            size_t dst_idx = h * K + rank;

            uint32_t total_c = 0;
            uint32_t total_e = 0;
            for (unsigned int t = 0; t < num_threads; ++t) {
                total_c += thread_counts[t][src_idx];
                total_e += thread_errors[t][src_idx];
            }
            g_counts[dst_idx] = total_c;
            g_errors[dst_idx] = total_e;
        }
    }

    std::vector<uint32_t> g_offsets(active_cells + 1, 0);
    uint32_t total_records = 0;
    for (size_t c = 0; c < active_cells; ++c) {
        g_offsets[c] = total_records;
        uint32_t running = total_records;
        size_t h = c / K;
        size_t rank = c % K;
        uint16_t old_id = sorted_order[rank];
        size_t src_idx = h * MAX_ENDPOINTS + old_id;
        for (unsigned int t = 0; t < num_threads; ++t) {
            thread_write_ptrs[t][c] = running;
            running += thread_counts[t][src_idx];
        }
        total_records += g_counts[c];
    }
    g_offsets[active_cells] = total_records;

    thread_counts.clear();
    thread_counts.shrink_to_fit();
    thread_errors.clear();
    thread_errors.shrink_to_fit();

    std::vector<uint32_t> g_flat(total_records);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            auto& write_ptrs = thread_write_ptrs[tid];
            const auto& my_chunks = thread_chunks[tid];
            alignas(64) char chunk_buf[CHUNK_SIZE + 4096];

            for (const auto& chk : my_chunks) {
#ifdef _WIN32
                ssize_t n = my_pread(hFile, chunk_buf, chk.len, chk.start);
#else
                ssize_t n = my_pread(fd, chunk_buf, chk.len, chk.start);
#endif
                if (n <= 0) continue;

                const char* cur = chunk_buf;
                const char* limit = chunk_buf + n;

                while (cur + 38 < limit) {
                    const char* line_start = cur;
                    const char* nl = static_cast<const char*>(std::memchr(cur + 38, '\n', limit - (cur + 38)));
                    if (!nl) break;
                    cur = nl + 1;

                    const char* q = nl - 1;
                    while (*q != ' ') q--;
                    const char* ep_end = q - 4;

                    uint8_t mlen = get_method_len(line_start + 28);
                    const char* ep_start = line_start + 28 + mlen;
                    uint16_t ep_len = (uint16_t)(ep_end - ep_start);

                    uint32_t hour_idx = parse_hour_idx(line_start);
                    uint16_t rank = dict_find_lex(ep_start, ep_len);

                    const char* p_lat = q + 1;
                    uint32_t lat = 0;
                    while (p_lat < nl) {
                        lat = lat * 10 + (*p_lat - '0');
                        p_lat++;
                    }

                    size_t cell = (size_t)hour_idx * K + rank;
                    uint32_t pos = write_ptrs[cell]++;
                    g_flat[pos] = lat;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    thread_write_ptrs.clear();
    thread_write_ptrs.shrink_to_fit();

    std::vector<std::string> hour_outputs(NUM_HOURS);
    size_t hours_per_thread = (NUM_HOURS + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            size_t h_start = tid * hours_per_thread;
            size_t h_end = std::min(NUM_HOURS, (tid + 1) * hours_per_thread);

            alignas(64) char line_buf[1024];

            for (size_t h = h_start; h < h_end; ++h) {
                std::string& h_out = hour_outputs[h];
                h_out.reserve(K * 80);

                char hour_str[21];
                fmt_hour(h, hour_str);

                for (uint16_t rank = 0; rank < K; ++rank) {
                    size_t c = h * K + rank;
                    uint32_t count = g_counts[c];
                    if (count == 0) continue;

                    uint32_t errors = g_errors[c];
                    uint32_t off = g_offsets[c];
                    uint32_t* lat_ptr = g_flat.data() + off;

                    uint32_t p50, p95, p99;
                    if (count <= 32) {
                        insertion_sort(lat_ptr, count);
                        p50 = lat_ptr[count / 2];
                        p95 = lat_ptr[(count * 95) / 100];
                        p99 = lat_ptr[(count * 99) / 100];
                    } else {
                        size_t i50 = count / 2;
                        size_t i95 = (count * 95) / 100;
                        size_t i99 = (count * 99) / 100;
                        std::nth_element(lat_ptr, lat_ptr + i50, lat_ptr + count);
                        p50 = lat_ptr[i50];
                        std::nth_element(lat_ptr + i50 + 1, lat_ptr + i95, lat_ptr + count);
                        p95 = lat_ptr[i95];
                        std::nth_element(lat_ptr + i95 + 1, lat_ptr + i99, lat_ptr + count);
                        p99 = lat_ptr[i99];
                    }

                    uint16_t old_id = sorted_order[rank];
                    const std::string& ep_str = g_endpoint_list[old_id];

                    char* p = line_buf;
                    std::memcpy(p, hour_str, 20);
                    p += 20;
                    *p++ = ' ';

                    std::memcpy(p, ep_str.data(), ep_str.size());
                    p += ep_str.size();
                    *p++ = ' ';

                    p = write_uint(p, count);
                    *p++ = ' ';
                    p = write_uint(p, errors);
                    *p++ = ' ';
                    p = write_uint(p, p50);
                    *p++ = ' ';
                    p = write_uint(p, p95);
                    *p++ = ' ';
                    p = write_uint(p, p99);
                    *p++ = '\n';

                    h_out.append(line_buf, p - line_buf);
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

    for (size_t h = 0; h < NUM_HOURS; ++h) {
        if (!hour_outputs[h].empty()) {
            fwrite(hour_outputs[h].data(), 1, hour_outputs[h].size(), fp);
        }
    }
    fclose(fp);

#ifdef _WIN32
    CloseHandle(hFile);
#else
    close(fd);
#endif
    return 0;
}
