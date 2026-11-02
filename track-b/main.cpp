#if defined(__GLIBC__) || defined(__linux__)
#include <malloc.h>
#endif
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <thread>
#include <chrono>
#include <array>
#include <memory>
#include <atomic>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

struct MappedFile {
    void*  data = nullptr;
    size_t size = 0;
    bool   ok   = false;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE, hMap = NULL;
    explicit MappedFile(const char* p) {
        hFile = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return; }
        size = (size_t)li.QuadPart;
        if (!size) { ok = true; return; }
        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        data = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        ok = (data != nullptr);
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }
#else
    explicit MappedFile(const char* p) {
        int fd = ::open(p, O_RDONLY);
        if (fd < 0) return;
        struct stat st;
        if (::fstat(fd, &st) != 0) { ::close(fd); return; }
        size = (size_t)st.st_size;
        if (!size) { ok = true; ::close(fd); return; }
        data = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (data == MAP_FAILED) { data = nullptr; size = 0; return; }
        ok = true;
        if (size >= (1u << 21)) ::madvise(data, size, MADV_SEQUENTIAL | MADV_WILLNEED);
    }
    ~MappedFile() { if (data) ::munmap(data, size); }
#endif
};

inline constexpr auto TOKEN_CHAR = [] {
    std::array<uint8_t, 256> t{};
    for (int c = '0'; c <= '9'; ++c) t[c] = (uint8_t)c;
    for (int c = 'a'; c <= 'z'; ++c) t[c] = (uint8_t)c;
    for (int c = 'A'; c <= 'Z'; ++c) t[c] = (uint8_t)(c + 32);
    return t;
}();

inline uint64_t fnv1a_str(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) { h ^= (uint8_t)c; h *= 0x100000001b3ULL; }
    return h ? h : 1;
}

static constexpr size_t VOCAB_CAP = 262144, VOCAB_MASK = VOCAB_CAP - 1;
struct VocabSlot { uint64_t hash = 0; uint32_t token_id = 0; };
static VocabSlot vocab_table[VOCAB_CAP];
static uint32_t num_active_tokens = 0;
static uint64_t bloom_bits[4096];

inline void bloom_set(uint64_t h) {
    bloom_bits[(h & 0x3FFFF) >> 6] |= (1ULL << (h & 63));
    bloom_bits[((h >> 18) & 0x3FFFF) >> 6] |= (1ULL << ((h >> 18) & 63));
}

inline bool bloom_test(uint64_t h) {
    uint64_t m1 = 1ULL << (h & 63);
    if (!(bloom_bits[(h & 0x3FFFF) >> 6] & m1)) return false;
    uint64_t m2 = 1ULL << ((h >> 18) & 63);
    return (bloom_bits[((h >> 18) & 0x3FFFF) >> 6] & m2) != 0;
}

static constexpr uint32_t BITSET_WORDS = 3908, BITSET_FREQ_THRESHOLD = 3500;
static constexpr uint64_t TAIL_MASK = (1ULL << 16) - 1;

template <typename T>
inline void append_uint(std::string& s, T val) {
    char buf[32]; char* p = buf + 32;
    if (!val) { s.push_back('0'); return; }
    while (val) { *--p = '0' + (val % 10); val /= 10; }
    s.append(p, (buf + 32) - p);
}

template <bool EQ, bool AO, bool BO>
inline size_t setop(const uint32_t* a, size_t na, const uint32_t* b, size_t nb, uint32_t* out) {
    size_t ia = 0, ib = 0, k = 0;
    while (ia < na && ib < nb) {
        uint32_t va = a[ia], vb = b[ib];
        if (va == vb) { if constexpr (EQ) out[k++] = va; ia++; ib++; }
        else if (va < vb) { if constexpr (AO) out[k++] = va; ia++; }
        else { if constexpr (BO) out[k++] = vb; ib++; }
    }
    if constexpr (AO) while (ia < na) out[k++] = a[ia++];
    if constexpr (BO) while (ib < nb) out[k++] = b[ib++];
    return k;
}

inline size_t intersect_small_large(const uint32_t* sm, size_t ns, const uint32_t* lg, size_t nl, uint32_t* out) {
    size_t j = 0, k = 0;
    for (size_t i = 0; i < ns && j < nl; ++i) {
        uint32_t tgt = sm[i], st = 1, r = j;
        while (r < nl && lg[r] < tgt) { j = r; r += st; st <<= 1; }
        if (r > nl) r = nl;
        const uint32_t* it = std::lower_bound(lg + j, lg + r, tgt);
        j = (size_t)(it - lg);
        if (j < nl && lg[j] == tgt) { out[k++] = tgt; ++j; }
    }
    return k;
}

inline bool match_phrase_kmp_fast(const char* p, const char* end, const std::string_view* terms,
                                  const uint64_t* exp_u64, const uint32_t* exp_len,
                                  const size_t* pi, size_t k) {
    if (!k) return true;
    size_t j = 0;
    while (p < end) {
        while (p < end && !TOKEN_CHAR[(uint8_t)*p]) ++p;
        if (p >= end) break;
        const char* start = p;
        while (p < end && TOKEN_CHAR[(uint8_t)*p]) ++p;
        uint32_t len = (uint32_t)(p - start);
        auto eq = [&](size_t idx) {
            if (len != exp_len[idx]) return false;
            if (len <= 8) {
                uint64_t w = 0;
                std::memcpy(&w, start, len);
                return (w | 0x2020202020202020ull) == exp_u64[idx];
            }
            const auto& exp = terms[idx];
            for (size_t x = 0; x < len; ++x) if ((start[x] | 0x20) != (exp[x] | 0x20)) return false;
            return true;
        };
        while (j > 0 && !eq(j)) j = pi[j - 1];
        if (eq(j)) ++j;
        if (j == k) return true;
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    MappedFile dmf((data_dir + "/docs.txt").c_str());
    if (!dmf.ok) return 1;
    const char* docs_data = (const char*)dmf.data;
    size_t docs_size = dmf.size;

    MappedFile qmf((data_dir + "/queries.txt").c_str());
    if (!qmf.ok) return 1;
    const char* qp = (const char*)qmf.data;
    const char* qend = qp + qmf.size;

    unsigned num_threads = std::thread::hardware_concurrency();
    if (!num_threads) num_threads = 6;
    num_threads = std::min(num_threads, 12u);

    std::vector<std::string_view> query_lines;
    query_lines.reserve(65536);

    auto insert_token = [&](std::string_view sv) -> uint32_t {
        uint64_t h = fnv1a_str(sv);
        bloom_set(h);
        size_t s = h & VOCAB_MASK;
        while (vocab_table[s].hash) {
            if (vocab_table[s].hash == h) return vocab_table[s].token_id;
            s = (s + 1) & VOCAB_MASK;
        }
        uint32_t nid = num_active_tokens++;
        vocab_table[s].hash = h;
        vocab_table[s].token_id = nid;
        return nid;
    };

    auto find_token = [&](std::string_view sv) -> int64_t {
        uint64_t h = fnv1a_str(sv);
        if (!bloom_test(h)) return -1;
        size_t s = h & VOCAB_MASK;
        while (vocab_table[s].hash) {
            if (vocab_table[s].hash == h) return vocab_table[s].token_id;
            s = (s + 1) & VOCAB_MASK;
        }
        return -1;
    };

    while (qp < qend) {
        const char* nl = (const char*)std::memchr(qp, '\n', qend - qp);
        const char* le = nl ? nl : qend;
        size_t len = le - qp;
        while (len > 0 && (qp[len - 1] == '\r' || qp[len - 1] == ' ' || qp[len - 1] == '\t')) len--;
        std::string_view qline(qp, len);
        query_lines.push_back(qline);

        if (qline.starts_with("BOOL ")) {
            size_t p = 5;
            while (p < qline.size()) {
                while (p < qline.size() && qline[p] == ' ') p++;
                if (p >= qline.size()) break;
                size_t sp = qline.find(' ', p);
                if (sp == std::string_view::npos) sp = qline.size();
                std::string_view w = qline.substr(p, sp - p);
                p = sp;
                if (w != "AND" && w != "OR" && w != "NOT") insert_token(w);
            }
        } else if (qline.starts_with("PHRASE \"")) {
            std::string_view in = qline.substr(8);
            if (!in.empty() && in.back() == '"') in.remove_suffix(1);
            size_t p = 0;
            while (p < in.size()) {
                while (p < in.size() && in[p] == ' ') p++;
                if (p >= in.size()) break;
                size_t sp = in.find(' ', p);
                if (sp == std::string_view::npos) sp = in.size();
                insert_token(in.substr(p, sp - p));
                p = sp;
            }
        }
        qp = nl ? nl + 1 : qend;
    }

    size_t chunk_bytes = docs_size / num_threads;
    std::vector<const char*> chunk_starts(num_threads), chunk_ends(num_threads);
    for (unsigned tid = 0; tid < num_threads; ++tid) {
        const char* s = docs_data + tid * chunk_bytes;
        const char* e = (tid == num_threads - 1) ? (docs_data + docs_size) : (docs_data + (tid + 1) * chunk_bytes);
        if (tid > 0) { while (s < docs_data + docs_size && *s != '\n') s++; if (s < docs_data + docs_size) s++; }
        if (tid < num_threads - 1) { while (e < docs_data + docs_size && *e != '\n') e++; if (e < docs_data + docs_size) e++; }
        chunk_starts[tid] = s; chunk_ends[tid] = e;
    }

    struct DocSpan { uint32_t off = 0, len = 0; };
    constexpr size_t MAX_DOCS = 262144;
    std::vector<DocSpan> doc_spans(MAX_DOCS);
    std::vector<uint32_t> thread_counts(num_threads * num_active_tokens, 0);

    // Single-pass text tokenization: cache intermediate tokens per thread
    struct ThreadDocEntry { uint32_t doc_id, tok_start, tok_count; };
    std::vector<std::vector<uint32_t>> thread_tokens(num_threads);
    std::vector<std::vector<ThreadDocEntry>> thread_docs(num_threads);
    std::vector<std::thread> workers;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* cur = chunk_starts[tid];
            const char* limit = chunk_ends[tid];
            uint32_t* counts = &thread_counts[tid * num_active_tokens];
            auto& t_toks = thread_tokens[tid];
            t_toks.reserve(1900000);
            auto& t_docs = thread_docs[tid];
            t_docs.reserve(24000);
            std::vector<uint32_t> last_doc(num_active_tokens, 0xFFFFFFFFu);

            while (cur < limit) {
                uint32_t cur_doc_id = 0;
                while (cur < limit && static_cast<unsigned char>(*cur - '0') <= 9) {
                    cur_doc_id = cur_doc_id * 10 + (*cur - '0');
                    cur++;
                }
                if (cur < limit && *cur == '\t') cur++;
                if (cur >= limit) break;

                const char* text_start = cur;
                const char* nl = (const char*)std::memchr(cur, '\n', limit - cur);
                const char* line_end = nl ? nl : limit;
                cur = nl ? nl + 1 : limit;

                const char* text_end = line_end;
                while (text_end > text_start && (text_end[-1] == '\r' || text_end[-1] == ' ' || text_end[-1] == '\t')) text_end--;

                if (cur_doc_id < MAX_DOCS) {
                    doc_spans[cur_doc_id] = {(uint32_t)(text_start - docs_data), (uint32_t)(text_end - text_start)};
                }

                uint32_t tok_start = (uint32_t)t_toks.size();
                const char* p = text_start;
                while (p < text_end) {
                    while (p < text_end && !TOKEN_CHAR[(uint8_t)*p]) p++;
                    if (p >= text_end) break;
                    uint64_t h = 0xcbf29ce484222325ULL;
                    while (p < text_end) {
                        uint8_t tc = TOKEN_CHAR[(uint8_t)*p];
                        if (!tc) break;
                        h = (h ^ tc) * 0x100000001b3ULL;
                        p++;
                    }
                    if (!h) h = 1;
                    if (!bloom_test(h)) continue;

                    size_t slot = h & VOCAB_MASK;
                    while (vocab_table[slot].hash) {
                        if (vocab_table[slot].hash == h) {
                            uint32_t tok_id = vocab_table[slot].token_id;
                            if (last_doc[tok_id] != cur_doc_id) {
                                last_doc[tok_id] = cur_doc_id;
                                counts[tok_id]++;
                                t_toks.push_back(tok_id);
                            }
                            break;
                        }
                        slot = (slot + 1) & VOCAB_MASK;
                    }
                }
                uint32_t tok_count = (uint32_t)t_toks.size() - tok_start;
                if (tok_count) t_docs.push_back({cur_doc_id, tok_start, tok_count});
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    std::vector<uint32_t> token_df(num_active_tokens, 0);
    size_t num_bitset_terms = 0;
    for (uint32_t tok_id = 0; tok_id < num_active_tokens; ++tok_id) {
        uint32_t total = 0;
        for (unsigned tid = 0; tid < num_threads; ++tid) total += thread_counts[tid * num_active_tokens + tok_id];
        token_df[tok_id] = total;
        if (total >= BITSET_FREQ_THRESHOLD) num_bitset_terms++;
    }

    std::vector<uint64_t> bitset_arena(num_bitset_terms * BITSET_WORDS, 0ULL);
    std::vector<uint64_t*> term_bitsets(num_active_tokens, nullptr);
    size_t cur_bitset_idx = 0;
    for (uint32_t tok_id = 0; tok_id < num_active_tokens; ++tok_id) {
        if (token_df[tok_id] >= BITSET_FREQ_THRESHOLD) {
            term_bitsets[tok_id] = bitset_arena.data() + cur_bitset_idx * BITSET_WORDS;
            cur_bitset_idx++;
        }
    }

    std::vector<uint32_t> csr_offsets(num_active_tokens + 1, 0);
    std::vector<uint32_t> thread_write_ptrs(num_threads * num_active_tokens, 0);
    uint32_t running_total = 0;
    for (uint32_t tok_id = 0; tok_id < num_active_tokens; ++tok_id) {
        csr_offsets[tok_id] = running_total;
        if (token_df[tok_id] < BITSET_FREQ_THRESHOLD) {
            uint32_t t_base = running_total;
            for (unsigned tid = 0; tid < num_threads; ++tid) {
                thread_write_ptrs[tid * num_active_tokens + tok_id] = t_base;
                t_base += thread_counts[tid * num_active_tokens + tok_id];
            }
            running_total = t_base;
        }
    }
    csr_offsets[num_active_tokens] = running_total;
    thread_counts.clear(); thread_counts.shrink_to_fit();
    std::vector<uint32_t> csr_doc_ids(running_total);

    // Pure in-memory array iteration (NO 2nd pass over 253 MB text!)
    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            uint32_t* write_ptrs = &thread_write_ptrs[tid * num_active_tokens];
            const auto& t_toks = thread_tokens[tid];
            const auto& t_docs = thread_docs[tid];

            for (const auto& doc : t_docs) {
                uint32_t cur_doc_id = doc.doc_id;
                uint32_t start = doc.tok_start;
                uint32_t count = doc.tok_count;
                for (uint32_t k = 0; k < count; ++k) {
                    uint32_t tok_id = t_toks[start + k];
                    uint64_t* bset = term_bitsets[tok_id];
                    if (bset) {
                        reinterpret_cast<std::atomic<uint64_t>*>(bset)[cur_doc_id >> 6].fetch_or(1ULL << (cur_doc_id & 63), std::memory_order_relaxed);
                    } else {
                        csr_doc_ids[write_ptrs[tok_id]++] = cur_doc_id;
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    thread_tokens.clear(); thread_tokens.shrink_to_fit();
    thread_docs.clear(); thread_docs.shrink_to_fit();
    thread_write_ptrs.clear(); thread_write_ptrs.shrink_to_fit();
#if defined(__GLIBC__) || defined(__linux__)
    malloc_trim(0);
#endif

    for (size_t i = 0; i < num_bitset_terms; ++i) {
        uint64_t* bits = bitset_arena.data() + i * BITSET_WORDS;
        bits[3906] &= TAIL_MASK; bits[3907] = 0ULL;
    }

    std::vector<std::string> thread_outputs(num_threads);
    size_t num_queries = query_lines.size();
    size_t queries_per_thread = (num_queries + num_threads - 1) / num_threads;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            size_t q_start = tid * queries_per_thread;
            size_t q_end = std::min(num_queries, (tid + 1) * queries_per_thread);
            if (q_start >= q_end) return;

            std::string& out_str = thread_outputs[tid];
            out_str.reserve(1024 * 1024);

            std::unique_ptr<uint32_t[]> buf_a(new uint32_t[262144]), buf_b(new uint32_t[262144]);
            std::unique_ptr<uint64_t[]> dense_scratch(new uint64_t[BITSET_WORDS]);
            std::vector<uint32_t> matches; matches.reserve(65536);
            std::string_view parts[32], phrase_terms[32];

            for (size_t qidx = q_start; qidx < q_end; ++qidx) {
                std::string_view qline = query_lines[qidx];

                if (qline.starts_with("BOOL ")) {
                    std::string_view rest = qline.substr(5);
                    size_t num_parts = 0, pos = 0;
                    while (pos < rest.size() && num_parts < 32) {
                        size_t next_sp = rest.find(' ', pos);
                        if (next_sp == std::string_view::npos) { parts[num_parts++] = rest.substr(pos); break; }
                        parts[num_parts++] = rest.substr(pos, next_sp - pos);
                        pos = next_sp + 1;
                    }
                    if (!num_parts) { out_str.append("0 0\n"); continue; }

                    int64_t id0 = find_token(parts[0]);
                    bool is_dense = false;
                    uint32_t* cur_buf = buf_a.get(), *next_buf = buf_b.get();
                    uint64_t* dense_buf = dense_scratch.get();
                    size_t cur_len = 0;

                    if (id0 >= 0 && (size_t)id0 < num_active_tokens) {
                        const uint64_t* bset0 = term_bitsets[id0];
                        if (bset0) {
                            std::memcpy(dense_buf, bset0, BITSET_WORDS * sizeof(uint64_t));
                            is_dense = true;
                        } else {
                            uint32_t start = csr_offsets[id0];
                            cur_len = csr_offsets[id0 + 1] - start;
                            if (cur_len) std::memcpy(cur_buf, csr_doc_ids.data() + start, cur_len * sizeof(uint32_t));
                        }
                    }

                    for (size_t i = 1; i + 1 < num_parts; i += 2) {
                        std::string_view op = parts[i], term = parts[i + 1];
                        int64_t post_id = find_token(term);
                        const uint32_t* post_data = nullptr;
                        size_t post_len = 0;
                        const uint64_t* post_bitset = nullptr;

                        if (post_id >= 0 && (size_t)post_id < num_active_tokens) {
                            post_bitset = term_bitsets[post_id];
                            if (!post_bitset) {
                                uint32_t start = csr_offsets[post_id];
                                post_len = csr_offsets[post_id + 1] - start;
                                post_data = csr_doc_ids.data() + start;
                            }
                        }

                        if (op == "AND") {
                            if (is_dense) {
                                if (post_bitset) {
                                    for (size_t w = 0; w < BITSET_WORDS; w += 4) {
                                        __m256i a = _mm256_loadu_si256((const __m256i*)(dense_buf + w));
                                        __m256i b = _mm256_loadu_si256((const __m256i*)(post_bitset + w));
                                        _mm256_storeu_si256((__m256i*)(dense_buf + w), _mm256_and_si256(a, b));
                                    }
                                } else {
                                    size_t next_len = 0;
                                    for (size_t k = 0; k < post_len; ++k) {
                                        uint32_t doc = post_data[k];
                                        if (dense_buf[doc >> 6] & (1ULL << (doc & 63))) cur_buf[next_len++] = doc;
                                    }
                                    cur_len = next_len; is_dense = false;
                                }
                            } else if (cur_len > 0) {
                                size_t next_len = 0;
                                if (post_bitset) {
                                    for (size_t k = 0; k < cur_len; ++k) {
                                        uint32_t doc = cur_buf[k];
                                        if (post_bitset[doc >> 6] & (1ULL << (doc & 63))) next_buf[next_len++] = doc;
                                    }
                                } else if (post_len > 0) {
                                    if (post_len * 8 < cur_len) next_len = intersect_small_large(post_data, post_len, cur_buf, cur_len, next_buf);
                                    else if (cur_len * 8 < post_len) next_len = intersect_small_large(cur_buf, cur_len, post_data, post_len, next_buf);
                                    else next_len = setop<true, false, false>(cur_buf, cur_len, post_data, post_len, next_buf);
                                }
                                cur_len = next_len; std::swap(cur_buf, next_buf);
                            }
                        } else if (op == "OR") {
                            if (is_dense) {
                                if (post_bitset) {
                                    for (size_t w = 0; w < BITSET_WORDS; w += 4) {
                                        __m256i a = _mm256_loadu_si256((const __m256i*)(dense_buf + w));
                                        __m256i b = _mm256_loadu_si256((const __m256i*)(post_bitset + w));
                                        _mm256_storeu_si256((__m256i*)(dense_buf + w), _mm256_or_si256(a, b));
                                    }
                                } else {
                                    for (size_t k = 0; k < post_len; ++k) {
                                        uint32_t doc = post_data[k]; dense_buf[doc >> 6] |= (1ULL << (doc & 63));
                                    }
                                }
                            } else {
                                if (post_bitset) {
                                    std::memcpy(dense_buf, post_bitset, BITSET_WORDS * sizeof(uint64_t));
                                    for (size_t k = 0; k < cur_len; ++k) {
                                        uint32_t doc = cur_buf[k]; dense_buf[doc >> 6] |= (1ULL << (doc & 63));
                                    }
                                    is_dense = true;
                                } else if (post_len > 0) {
                                    size_t next_len = setop<true, true, true>(cur_buf, cur_len, post_data, post_len, next_buf);
                                    cur_len = next_len; std::swap(cur_buf, next_buf);
                                }
                            }
                        } else if (op == "NOT") {
                            if (is_dense) {
                                if (post_bitset) {
                                    for (size_t w = 0; w < BITSET_WORDS; w += 4) {
                                        __m256i a = _mm256_loadu_si256((const __m256i*)(dense_buf + w));
                                        __m256i b = _mm256_loadu_si256((const __m256i*)(post_bitset + w));
                                        _mm256_storeu_si256((__m256i*)(dense_buf + w), _mm256_andnot_si256(b, a));
                                    }
                                } else {
                                    for (size_t k = 0; k < post_len; ++k) {
                                        uint32_t doc = post_data[k]; dense_buf[doc >> 6] &= ~(1ULL << (doc & 63));
                                    }
                                }
                            } else if (cur_len > 0) {
                                size_t next_len = 0;
                                if (post_bitset) {
                                    for (size_t k = 0; k < cur_len; ++k) {
                                        uint32_t doc = cur_buf[k];
                                        if (!(post_bitset[doc >> 6] & (1ULL << (doc & 63)))) next_buf[next_len++] = doc;
                                    }
                                    cur_len = next_len; std::swap(cur_buf, next_buf);
                                } else if (post_len > 0) {
                                    next_len = setop<false, true, false>(cur_buf, cur_len, post_data, post_len, next_buf);
                                    cur_len = next_len; std::swap(cur_buf, next_buf);
                                }
                            }
                        }
                    }

                    if (is_dense) {
                        dense_buf[3906] &= TAIL_MASK; dense_buf[3907] = 0ULL;
                        uint32_t total_matches = 0; uint64_t sum_ids = 0;
                        uint32_t first10[10]; size_t first10_count = 0;
                        for (size_t w = 0; w < BITSET_WORDS; ++w) {
                            uint64_t word = dense_buf[w];
                            if (!word) continue;
                            total_matches += (uint32_t)std::popcount(word);
                            uint32_t base_id = (uint32_t)w << 6;
                            while (word) {
                                uint32_t b = (uint32_t)__builtin_ctzll(word);
                                uint32_t doc_id = base_id + b;
                                sum_ids += doc_id;
                                if (first10_count < 10) first10[first10_count++] = doc_id;
                                word &= word - 1;
                            }
                        }
                        if (!total_matches) { out_str.append("0 0\n"); }
                        else {
                            append_uint(out_str, total_matches); out_str.push_back(' ');
                            append_uint(out_str, sum_ids);
                            for (size_t k = 0; k < first10_count; ++k) { out_str.push_back(' '); append_uint(out_str, first10[k]); }
                            out_str.push_back('\n');
                        }
                    } else {
                        if (!cur_len) { out_str.append("0 0\n"); }
                        else {
                            uint64_t sum_ids = 0;
                            for (size_t k = 0; k < cur_len; ++k) sum_ids += cur_buf[k];
                            append_uint(out_str, cur_len); out_str.push_back(' ');
                            append_uint(out_str, sum_ids);
                            size_t emit_count = std::min(cur_len, (size_t)10);
                            for (size_t k = 0; k < emit_count; ++k) { out_str.push_back(' '); append_uint(out_str, cur_buf[k]); }
                            out_str.push_back('\n');
                        }
                    }
                } else if (qline.starts_with("PHRASE \"")) {
                    std::string_view in = qline.substr(8);
                    if (!in.empty() && in.back() == '"') in.remove_suffix(1);
                    size_t num_phrase_terms = 0, pos = 0, min_phrase_len = 0;
                    while (pos < in.size() && num_phrase_terms < 32) {
                        while (pos < in.size() && in[pos] == ' ') pos++;
                        if (pos >= in.size()) break;
                        size_t sp = in.find(' ', pos);
                        if (sp == std::string_view::npos) sp = in.size();
                        std::string_view w = in.substr(pos, sp - pos);
                        min_phrase_len += w.size();
                        phrase_terms[num_phrase_terms++] = w;
                        pos = sp;
                    }
                    if (!num_phrase_terms) { out_str.append("0 0\n"); continue; }

                    struct PhraseTermInfo { size_t term_idx, len; int64_t tok_id; };
                    PhraseTermInfo pinfo[32]; bool missing = false;
                    for (size_t i = 0; i < num_phrase_terms; ++i) {
                        int64_t tid_val = find_token(phrase_terms[i]);
                        if (tid_val < 0 || (size_t)tid_val >= num_active_tokens) { missing = true; break; }
                        uint32_t plen = token_df[tid_val];
                        if (!plen) { missing = true; break; }
                        pinfo[i] = {i, plen, tid_val};
                    }
                    if (missing) { out_str.append("0 0\n"); continue; }

                    size_t k_lim = std::min(num_phrase_terms, (size_t)8), pi[8] = {};
                    for (size_t i = 1, j = 0; i < k_lim; ++i) {
                        while (j > 0 && phrase_terms[i] != phrase_terms[j]) j = pi[j - 1];
                        if (phrase_terms[i] == phrase_terms[j]) ++j;
                        pi[i] = j;
                    }
                    uint64_t exp_u64[8] = {};
                    uint32_t exp_len[8] = {};
                    for (size_t i = 0; i < k_lim; ++i) {
                        exp_len[i] = (uint32_t)phrase_terms[i].size();
                        if (exp_len[i] <= 8) {
                            uint64_t e = 0;
                            std::memcpy(&e, phrase_terms[i].data(), exp_len[i]);
                            exp_u64[i] = e | 0x2020202020202020ull;
                        }
                    }
                    std::sort(pinfo, pinfo + num_phrase_terms, [](const auto& a, const auto& b) { return a.len < b.len; });

                    uint32_t* cur_cand = buf_a.get(), *next_cand = buf_b.get();
                    uint32_t first_tok_id = pinfo[0].tok_id;
                    size_t cur_cand_len = 0;

                    if (term_bitsets[first_tok_id]) {
                        const uint64_t* bset = term_bitsets[first_tok_id];
                        for (size_t w = 0; w < BITSET_WORDS; ++w) {
                            uint64_t word = bset[w];
                            uint32_t base_id = (uint32_t)w << 6;
                            while (word) {
                                cur_cand[cur_cand_len++] = base_id + (uint32_t)__builtin_ctzll(word);
                                word &= word - 1;
                            }
                        }
                    } else {
                        uint32_t f_start = csr_offsets[first_tok_id];
                        cur_cand_len = pinfo[0].len;
                        std::memcpy(cur_cand, csr_doc_ids.data() + f_start, cur_cand_len * sizeof(uint32_t));
                    }

                    for (size_t i = 1; i < num_phrase_terms && cur_cand_len > 0; ++i) {
                        if (pinfo[i].tok_id == pinfo[i - 1].tok_id) continue;
                        uint32_t t_id = pinfo[i].tok_id;
                        const uint64_t* p_bitset = term_bitsets[t_id];
                        size_t next_len = 0;
                        if (p_bitset) {
                            for (size_t k = 0; k < cur_cand_len; ++k) {
                                uint32_t doc = cur_cand[k];
                                if (p_bitset[doc >> 6] & (1ULL << (doc & 63))) next_cand[next_len++] = doc;
                            }
                        } else {
                            uint32_t start = csr_offsets[t_id];
                            size_t post_len = pinfo[i].len;
                            if (cur_cand_len * 8 < post_len) next_len = intersect_small_large(cur_cand, cur_cand_len, csr_doc_ids.data() + start, post_len, next_cand);
                            else next_len = setop<true, false, false>(cur_cand, cur_cand_len, csr_doc_ids.data() + start, post_len, next_cand);
                        }
                        cur_cand_len = next_len; std::swap(cur_cand, next_cand);
                    }

                    matches.clear(); uint64_t sum_ids = 0;
                    for (size_t k = 0; k < cur_cand_len; ++k) {
                        uint32_t doc_id = cur_cand[k];
                        if (doc_id < MAX_DOCS) {
                            const auto& span = doc_spans[doc_id];
                            if (span.len >= min_phrase_len) {
                                if (match_phrase_kmp_fast(docs_data + span.off, docs_data + span.off + span.len, phrase_terms, exp_u64, exp_len, pi, k_lim)) {
                                    matches.push_back(doc_id); sum_ids += doc_id;
                                }
                            }
                        }
                    }

                    if (matches.empty()) { out_str.append("0 0\n"); }
                    else {
                        append_uint(out_str, matches.size()); out_str.push_back(' ');
                        append_uint(out_str, sum_ids);
                        size_t emit_count = std::min(matches.size(), (size_t)10);
                        for (size_t k = 0; k < emit_count; ++k) { out_str.push_back(' '); append_uint(out_str, matches[k]); }
                        out_str.push_back('\n');
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();

#ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
#endif
    FILE* fp = fopen((out_dir + "/results.txt").c_str(), "wb");
    if (!fp) return 1;
    for (unsigned tid = 0; tid < num_threads; ++tid) fwrite(thread_outputs[tid].data(), 1, thread_outputs[tid].size(), fp);
    fclose(fp);

#ifndef _WIN32
    _exit(0);
#endif
    return 0;
}
