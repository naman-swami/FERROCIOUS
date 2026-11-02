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
#include <sys/mman.h>
#include <sched.h>
#endif

namespace {

constexpr size_t NUM_HOURS = 72;
constexpr size_t MAX_ENDPOINTS = 2048;
constexpr size_t NUM_CELLS = NUM_HOURS * MAX_ENDPOINTS;
constexpr size_t DICT_CAPACITY = 4096;
constexpr size_t DICT_MASK = DICT_CAPACITY - 1;
constexpr uint16_t DICT_CLAIMED = 0xFFFF;

inline void fmt_hour(char* out, uint32_t h) {
    uint32_t tot_h = 18 + h;
    uint32_t d_idx = tot_h / 24;
    uint32_t hr = tot_h % 24;
    uint32_t month = (d_idx < 2) ? 10 : 11;
    uint32_t day   = (d_idx < 2) ? (30 + d_idx) : (d_idx - 1);

    out[0] = '2'; out[1] = '0'; out[2] = '2'; out[3] = '6'; out[4] = '-';
    out[5] = (char)('0' + month / 10);
    out[6] = (char)('0' + month % 10);
    out[7] = '-';
    out[8] = (char)('0' + day / 10);
    out[9] = (char)('0' + day % 10);
    out[10] = 'T';
    out[11] = (char)('0' + hr / 10);
    out[12] = (char)('0' + hr % 10);
    out[13] = ':'; out[14] = '0'; out[15] = '0'; out[16] = ':'; out[17] = '0'; out[18] = '0'; out[19] = 'Z';
}

inline uint32_t parse_hour_idx(const char* p) {
    uint8_t m_char = p[6];
    uint32_t day = (p[8] - '0') * 10 + (p[9] - '0');
    uint32_t hr  = (p[11] - '0') * 10 + (p[12] - '0');
    uint32_t day_offset = (m_char == '0') ? (day - 30) : (day + 1);
    return day_offset * 24 + hr - 18;
}

inline uint8_t get_method_len(const char* p) {
    char c = p[0];
    if (c == 'G') return 4;
    if (c == 'P') {
        char c1 = p[1];
        if (c1 == 'U') return 4;
        if (c1 == 'O') return 5;
        return 6;
    }
    if (c == 'H') return 5;
    if (c == 'D') return 7;
    return 8;
}

inline uint32_t hash_endpoint(const char* p, uint16_t len, uint64_t prefix) {
    uint64_t h = (uint64_t)len * 0x517cc1b727220a95ULL ^ prefix;
    if (len >= 8) {
        uint64_t b;
        std::memcpy(&b, p + len - 8, 8);
        h = (h ^ (h >> 31)) * 0x9e3779b97f4a7c15ULL;
        h ^= b;
    }
    uint32_t res = (uint32_t)(h ^ (h >> 32));
    return res ? res : 1;
}

struct alignas(32) DictSlot {
    uint64_t prefix = 0;
    uint32_t hash = 0;
    uint32_t offset = 0;
    uint16_t len = 0;
    std::atomic<uint16_t> id{0};
};

alignas(64) static DictSlot g_dict[DICT_CAPACITY];
alignas(64) static char g_str_pool[DICT_CAPACITY * 128];
alignas(64) static std::atomic<uint32_t> g_str_pool_ptr{0};
alignas(64) static std::atomic<uint16_t> g_num_endpoints{0};

static std::string_view g_endpoint_list[MAX_ENDPOINTS];
static uint16_t g_old_to_lex[MAX_ENDPOINTS];

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
        uint16_t cur_id = slot.id.load(std::memory_order_acquire);
        if (cur_id == 0) {
            uint16_t expected = 0;
            if (slot.id.compare_exchange_strong(expected, DICT_CLAIMED, std::memory_order_acq_rel)) {
                uint16_t new_id = g_num_endpoints.fetch_add(1, std::memory_order_relaxed);
                uint32_t off = g_str_pool_ptr.fetch_add(len + 1, std::memory_order_relaxed);
                std::memcpy(g_str_pool + off, p, len);
                g_str_pool[off + len] = '\0';

                slot.prefix = prefix;
                slot.hash = h;
                slot.offset = off;
                slot.len = len;
                g_endpoint_list[new_id] = std::string_view(g_str_pool + off, len);

                slot.id.store(new_id + 1, std::memory_order_release);
                return new_id;
            }
            cur_id = slot.id.load(std::memory_order_acquire);
        }

        while (cur_id == DICT_CLAIMED) {
            #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
            #endif
            cur_id = slot.id.load(std::memory_order_acquire);
        }

        if (slot.hash == h && slot.len == len && slot.prefix == prefix) {
            if (len <= 8 || std::memcmp(g_str_pool + slot.offset + 8, p + 8, len - 8) == 0) {
                return cur_id - 1;
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

    #ifdef _WIN32
    HANDLE hFile = CreateFileA(input_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 1;
    LARGE_INTEGER liSize;
    GetFileSizeEx(hFile, &liSize);
    size_t total_size = liSize.QuadPart;
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    const char* data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, total_size);
    #else
    int fd = open(input_path.c_str(), O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    fstat(fd, &st);
    size_t total_size = st.st_size;
    const char* data = (const char*)mmap(NULL, total_size, PROT_READ, MAP_SHARED, fd, 0);
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
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

                while (cur + 38 < limit) {
                    const char* line_start = cur;
                    const char* nl = static_cast<const char*>(std::memchr(cur + 38, '\n', limit - (cur + 38)));
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

    // Thread-local write offsets: ZERO-ATOMIC scatter
    std::vector<std::vector<uint32_t>> thread_write_ptrs(num_threads, std::vector<uint32_t>(active_cells, 0));

    uint32_t hours_per_tid = (NUM_HOURS + num_threads - 1) / num_threads;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            uint32_t h_start = tid * hours_per_tid;
            uint32_t h_end = std::min((uint32_t)NUM_HOURS, (tid + 1) * hours_per_tid);
            for (uint32_t h = h_start; h < h_end; ++h) {
                size_t dst_base = (size_t)h * K;
                size_t src_base = (size_t)h * MAX_ENDPOINTS;
                for (uint16_t old_id = 0; old_id < K; ++old_id) {
                    uint16_t rank = g_old_to_lex[old_id];
                    size_t c = dst_base + rank;
                    uint32_t total_c = 0;
                    uint32_t total_err = 0;
                    for (unsigned int t = 0; t < num_threads; ++t) {
                        uint32_t cnt = thread_counts[t][src_base + old_id];
                        if (cnt > 0) {
                            total_c += cnt;
                            total_err += thread_errors[t][src_base + old_id];
                        }
                    }
                    g_counts[c] = total_c;
                    g_errors[c] = total_err;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

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

    // PASS 2: ZERO-ATOMIC Scatter directly into precomputed thread offsets
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            auto& write_ptrs = thread_write_ptrs[tid];
            const auto& my_chunks = thread_chunks[tid];
            for (const auto& chk : my_chunks) {
                const char* cur = data + chk.start;
                const char* limit = data + chk.end;

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
                        lat = lat * 10 + (*p_lat++ - '0');
                    }

                    size_t cell = (size_t)hour_idx * K + rank;
                    uint32_t slot = write_ptrs[cell]++;
                    g_flat[slot] = lat;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    thread_write_ptrs.clear();
    thread_write_ptrs.shrink_to_fit();

    // PASS 3: O(N) std::nth_element & Direct buffer format
    struct ThreadOutBuf {
        std::vector<char> buf;
        size_t len = 0;
    };
    std::vector<ThreadOutBuf> thread_out_bufs(num_threads);

    for (unsigned int h_tid = 0; h_tid < num_threads; ++h_tid) {
        workers.emplace_back([&, h_tid]() {
            uint32_t h_start = h_tid * hours_per_tid;
            uint32_t h_end = std::min((uint32_t)NUM_HOURS, (h_tid + 1) * hours_per_tid);
            if (h_start >= h_end) return;

            auto& out = thread_out_bufs[h_tid];
            out.buf.resize(2 * 1024 * 1024);
            char* ptr = out.buf.data();

            for (uint32_t h = h_start; h < h_end; ++h) {
                char hour_str[21];
                fmt_hour(hour_str, h);
                size_t base = (size_t)h * K;

                for (uint16_t rank = 0; rank < K; ++rank) {
                    size_t c = base + rank;
                    uint32_t count = g_counts[c];
                    if (count == 0) continue;

                    uint32_t off = g_offsets[c];
                    uint32_t* slice = g_flat.data() + off;

                    uint32_t r50 = (50u * count + 99u) / 100u - 1;
                    uint32_t r95 = (95u * count + 99u) / 100u - 1;
                    uint32_t r99 = (99u * count + 99u) / 100u - 1;

                    uint32_t p50, p95, p99;
                    if (count <= 32) {
                        insertion_sort(slice, count);
                        p50 = slice[r50];
                        p95 = slice[r95];
                        p99 = slice[r99];
                    } else {
                        std::nth_element(slice, slice + r50, slice + count);
                        p50 = slice[r50];
                        std::nth_element(slice + r50 + 1, slice + r95, slice + count);
                        p95 = slice[r95];
                        std::nth_element(slice + r95 + 1, slice + r99, slice + count);
                        p99 = slice[r99];
                    }

                    uint32_t errs = g_errors[c];
                    uint64_t ppm = (static_cast<uint64_t>(errs) * 1000000ULL) / count;
                    std::string_view ep_sv = g_endpoint_list[sorted_order[rank]];

                    std::memcpy(ptr, hour_str, 20);
                    ptr += 20;
                    *ptr++ = ' ';

                    std::memcpy(ptr, ep_sv.data(), ep_sv.size());
                    ptr += ep_sv.size();
                    *ptr++ = ' ';

                    ptr = write_uint(ptr, count);
                    *ptr++ = ' ';
                    ptr = write_uint(ptr, p50);
                    *ptr++ = ' ';
                    ptr = write_uint(ptr, p95);
                    *ptr++ = ' ';
                    ptr = write_uint(ptr, p99);
                    *ptr++ = ' ';
                    ptr = write_uint(ptr, errs);
                    *ptr++ = ' ';
                    ptr = write_uint(ptr, ppm);
                    *ptr++ = '\n';
                }
            }
            out.len = ptr - out.buf.data();
        });
    }
    for (auto& w : workers) w.join();

    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) return 1;

    for (unsigned int h_tid = 0; h_tid < num_threads; ++h_tid) {
        if (thread_out_bufs[h_tid].len > 0) {
            fwrite(thread_out_bufs[h_tid].buf.data(), 1, thread_out_bufs[h_tid].len, fp);
        }
    }
    fclose(fp);

    return 0;
}
