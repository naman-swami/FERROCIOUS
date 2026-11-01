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

constexpr size_t NUM_HOURS = 72;
constexpr size_t MAX_ENDPOINTS = 2048;
constexpr size_t NUM_CELLS = NUM_HOURS * MAX_ENDPOINTS; // 147,456
constexpr size_t DICT_CAPACITY = 4096;
constexpr size_t DICT_MASK = DICT_CAPACITY - 1;
constexpr uint16_t DICT_CLAIMED = 0xFFFF;

inline uint32_t parse_hour_idx(const char* p) {
    uint8_t m_char = p[6]; // '0' for Oct (10), '1' for Nov (11)
    uint32_t day = (p[8] - '0') * 10 + (p[9] - '0');
    uint32_t hr  = (p[11] - '0') * 10 + (p[12] - '0');
    uint32_t day_offset = (m_char == '0') ? (day - 30) : (day + 1);
    return day_offset * 24 + hr - 18;
}

inline uint8_t get_method_len(const char* p) {
    char c = p[0];
    if (c == 'G') return 4; // GET 
    if (c == 'P') {
        char c1 = p[1];
        if (c1 == 'U') return 4; // PUT 
        if (c1 == 'O') return 5; // POST 
        return 6; // PATCH 
    }
    if (c == 'H') return 5; // HEAD 
    if (c == 'D') return 7; // DELETE 
    return 8; // OPTIONS 
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
    uint64_t prefix = 0; // First 8 chars
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

    // PASS 1: Skip 38 bytes, backward space scan + Lock-Free Dictionary
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

                #ifndef _WIN32
                size_t page_start = start & ~4095ULL;
                size_t page_len = ((end + 4095ULL) & ~4095ULL) - page_start;
                madvise((void*)(data + page_start), page_len, MADV_DONTNEED);
                #endif
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // BARRIER: Lexicographical sort of endpoints
    uint16_t K = g_num_endpoints.load();
    std::vector<uint16_t> sorted_order(K);
    for (uint16_t i = 0; i < K; ++i) sorted_order[i] = i;

    std::sort(sorted_order.begin(), sorted_order.end(), [&](uint16_t a, uint16_t b) {
        return g_endpoint_list[a] < g_endpoint_list[b];
    });

    for (uint16_t rank = 0; rank < K; ++rank) {
        g_old_to_lex[sorted_order[rank]] = rank;
    }

    // Remap g_dict slots
    for (size_t i = 0; i < DICT_CAPACITY; ++i) {
        if (g_dict[i].id.load() > 0 && g_dict[i].id.load() <= K) {
            uint16_t old_id = g_dict[i].id.load() - 1;
            g_dict[i].id.store(g_old_to_lex[old_id] + 1, std::memory_order_relaxed);
        }
    }

    // Parallel merge into lex order across threads by hour chunks
    size_t active_cells = NUM_HOURS * K;
    std::vector<uint32_t> g_counts(active_cells, 0);
    std::vector<uint32_t> g_errors(active_cells, 0);

    uint32_t hours_per_tid = (NUM_HOURS + num_threads - 1) / num_threads;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            uint32_t h_start = tid * hours_per_tid;
            uint32_t h_end = std::min((uint32_t)NUM_HOURS, (tid + 1) * hours_per_tid);
            for (uint32_t h = h_start; h < h_end; ++h) {
                size_t dst_base = (size_t)h * K;
                size_t src_base = (size_t)h * MAX_ENDPOINTS;
                for (unsigned int t = 0; t < num_threads; ++t) {
                    const auto& c_vec = thread_counts[t];
                    const auto& e_vec = thread_errors[t];
                    for (uint16_t old_id = 0; old_id < K; ++old_id) {
                        uint32_t cnt = c_vec[src_base + old_id];
                        if (cnt > 0) {
                            uint16_t rank = g_old_to_lex[old_id];
                            size_t c = dst_base + rank;
                            g_counts[c] += cnt;
                            g_errors[c] += e_vec[src_base + old_id];
                        }
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    thread_counts.clear();
    thread_counts.shrink_to_fit();
    thread_errors.clear();
    thread_errors.shrink_to_fit();

    // Prefix sums
    std::vector<uint32_t> g_offsets(active_cells + 1, 0);
    std::vector<std::atomic<uint32_t>> g_cursors(active_cells);

    uint32_t total_records = 0;
    for (size_t c = 0; c < active_cells; ++c) {
        g_offsets[c] = total_records;
        g_cursors[c].store(total_records, std::memory_order_relaxed);
        total_records += g_counts[c];
    }
    g_offsets[active_cells] = total_records;

    // Allocate 12.4 MB flat latencies
    std::vector<uint32_t> g_flat(total_records);

    // PASS 2: Skip 38 bytes + forward latency parse + scatter to flat array
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
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
                    uint32_t slot = g_cursors[cell].fetch_add(1, std::memory_order_relaxed);
                    g_flat[slot] = lat;
                }

                #ifndef _WIN32
                size_t page_start = chk.start & ~4095ULL;
                size_t page_len = ((chk.end + 4095ULL) & ~4095ULL) - page_start;
                madvise((void*)(data + page_start), page_len, MADV_DONTNEED);
                #endif
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // PASS 3: Sort & Direct buffer format
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
                const std::string& hour_str = HOURS[h];
                size_t base = (size_t)h * K;

                for (uint16_t rank = 0; rank < K; ++rank) {
                    size_t c = base + rank;
                    uint32_t count = g_counts[c];
                    if (count == 0) continue;

                    uint32_t off = g_offsets[c];
                    uint32_t* slice = g_flat.data() + off;

                    if (count <= 32) {
                        insertion_sort(slice, count);
                    } else {
                        std::sort(slice, slice + count);
                    }

                    uint32_t p50 = slice[(50u * count + 99u) / 100u - 1];
                    uint32_t p95 = slice[(95u * count + 99u) / 100u - 1];
                    uint32_t p99 = slice[(99u * count + 99u) / 100u - 1];
                    uint32_t errs = g_errors[c];
                    uint64_t ppm = (static_cast<uint64_t>(errs) * 1000000ULL) / count;

                    std::string_view ep_sv = g_endpoint_list[sorted_order[rank]];

                    std::memcpy(ptr, hour_str.data(), 20);
                    ptr += 20;
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
