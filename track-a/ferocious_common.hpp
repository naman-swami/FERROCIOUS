// FERROCIOUS — Shared Systems Infrastructure. Zero Dependencies. C++20. AVX2.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <immintrin.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ferocious {

// ---------------------------------------------------------------- mmap RAII
struct MappedFile {
    void*    data = nullptr;
    size_t   size = 0;
    bool     ok   = false;
#ifdef _WIN32
    HANDLE   hFile = INVALID_HANDLE_VALUE;
    HANDLE   hMap  = NULL;
#endif

    explicit MappedFile(const char* path) {
#ifdef _WIN32
        hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return; }
        size = (size_t)li.QuadPart;
        if (size == 0) { ok = true; return; }
        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMap) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return; }
        data = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (!data) { CloseHandle(hMap); hMap = NULL; CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return; }
        ok = true;
#else
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return;
        struct stat st;
        if (::fstat(fd, &st) != 0) { ::close(fd); return; }
        size = (size_t)st.st_size;
        if (size == 0) { ok = true; ::close(fd); return; }
        data = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (data == MAP_FAILED) { data = nullptr; size = 0; return; }
        ok = true;
        if (size >= (1u << 21))
            ::madvise(data, size, MADV_SEQUENTIAL | MADV_WILLNEED);
#endif
    }

    ~MappedFile() {
#ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
        if (data) ::munmap(data, size);
#endif
    }

    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    const char* cdata() const { return static_cast<const char*>(data); }
};

// ------------------------------------------------------- fixed worker pool
class Parallel {
public:
    explicit Parallel(unsigned threads = 0)
        : nthreads_(threads ? threads : default_threads()) {}

    template <class F>
    void for_chunks(uint64_t nchunks, F&& fn) const {
        if (nchunks == 0) return;
        std::atomic<uint64_t> next{0};
        std::vector<std::thread> pool;
        unsigned t = (unsigned)std::min<uint64_t>(nthreads_, nchunks);
        for (unsigned i = 1; i < t; ++i)
            pool.emplace_back([&next, nchunks, &fn] { grab_loop(next, nchunks, fn); });
        grab_loop(next, nchunks, fn);
        for (auto& th : pool) th.join();
    }
    unsigned threads() const { return nthreads_; }

private:
    static unsigned default_threads() {
        unsigned hw = std::thread::hardware_concurrency();
        return hw ? std::min(hw, 12u) : 12u;
    }
    template <class F>
    static void grab_loop(std::atomic<uint64_t>& next, uint64_t n, F& fn) {
        for (;;) {
            uint64_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) return;
            fn(i);
        }
    }
    unsigned nthreads_;
};

// --------------------------------------------------------------- hashing
static inline uint32_t hash32(const char* p, size_t len) noexcept {
    uint32_t h = 0x811C9DC5u ^ (uint32_t)len;
    for (size_t i = 0; i < len; ++i) { h ^= (uint8_t)p[i]; h *= 0x01000193u; }
    h ^= h >> 16; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return h;
}

static inline uint64_t hash64(const char* p, size_t len) noexcept {
    uint64_t h = 0xCBF29CE484222325ull ^ len;
    for (size_t i = 0; i < len; ++i) { h ^= (uint8_t)p[i]; h *= 0x100000001B3ull; }
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 33;
    return h;
}

// --------------------------------------------------- AVX2 newline scanner
static inline size_t find_newlines_avx2(const char* ptr, size_t len,
                                        uint32_t* out, size_t max_out) noexcept {
    size_t count = 0, i = 0;
    const __m256i nl = _mm256_set1_epi8('\n');
    while (i < len && count < max_out &&
           ((reinterpret_cast<uintptr_t>(ptr + i) & 31) != 0)) {
        if (ptr[i] == '\n') out[count++] = (uint32_t)i;
        ++i;
    }
    for (; i + 32 <= len && count < max_out; i += 32) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)(ptr + i));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, nl));
        while (mask && count < max_out) {
            uint32_t bit = (uint32_t)__builtin_ctz(mask);
            out[count++] = (uint32_t)(i + bit);
            mask &= mask - 1;
        }
    }
    for (; i < len && count < max_out; ++i)
        if (ptr[i] == '\n') out[count++] = (uint32_t)i;
    return count;
}

// ------------------------------------------------------- buffered writer
struct OutBuf {
    FILE* f;
    std::vector<char> b;
    size_t n = 0;
    explicit OutBuf(FILE* f) : f(f), b(1 << 20) {}
    ~OutBuf() { flush(); }
    void flush() { if (n && f) { fwrite(b.data(), 1, n, f); n = 0; } }
    inline void put(char c) { b[n++] = c; if (n == b.size()) flush(); }
    inline void puts(const char* s, size_t len) {
        for (size_t i = 0; i < len; ++i) put(s[i]);
    }
    inline void u64(uint64_t v) {
        char t[24]; int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
    }
};

static inline uint32_t next_pow2(uint32_t v) noexcept {
    if (v <= 1) return 1;
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

} // namespace ferocious
