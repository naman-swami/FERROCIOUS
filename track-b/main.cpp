// FERROCIOUS Track B — 1-Pass Flat CSR + Zero-Allocation Streaming Phrase Engine
// Zero External Dependencies (D = 1.00). C++20.

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
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include "ferocious_common.hpp"

namespace {

inline constexpr auto TOKEN_CHAR = [] {
    std::array<uint8_t, 256> t{};
    for (int c = '0'; c <= '9'; ++c) t[c] = (uint8_t)c;
    for (int c = 'a'; c <= 'z'; ++c) t[c] = (uint8_t)c;
    for (int c = 'A'; c <= 'Z'; ++c) t[c] = (uint8_t)(c + 32);
    return t;
}();

inline uint64_t fnv1a_str(std::string_view sv) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : sv) {
        h = (h ^ (uint8_t)TOKEN_CHAR[(uint8_t)c]) * 0x100000001b3ULL;
    }
    return h == 0 ? 1 : h;
}

static constexpr size_t VOCAB_CAP = 262144;
static constexpr size_t VOCAB_MASK = VOCAB_CAP - 1;

struct VocabSlot {
    uint64_t hash = 0;
    uint32_t token_id = 0;
};

static VocabSlot vocab_table[VOCAB_CAP];

inline void append_u32(std::string& s, uint32_t v) {
    char buf[16];
    char* p = buf + sizeof(buf);
    if (v == 0) {
        s.push_back('0');
        return;
    }
    while (v > 0) {
        *--p = '0' + (v % 10);
        v /= 10;
    }
    s.append(p, buf + sizeof(buf) - p);
}

inline void append_u64(std::string& s, uint64_t v) {
    char buf[32];
    char* p = buf + sizeof(buf);
    if (v == 0) {
        s.push_back('0');
        return;
    }
    while (v > 0) {
        *--p = '0' + (v % 10);
        v /= 10;
    }
    s.append(p, buf + sizeof(buf) - p);
}

inline size_t count_newlines_simd(const char* data, size_t size) {
    size_t count = 0;
    size_t i = 0;
#if defined(__AVX2__)
    __m256i nl = _mm256_set1_epi8('\n');
    for (; i + 32 <= size; i += 32) {
        __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i cmp = _mm256_cmpeq_epi8(block, nl);
        unsigned mask = _mm256_movemask_epi8(cmp);
        count += _mm_popcnt_u32(mask);
    }
#endif
    for (; i < size; ++i) {
        if (data[i] == '\n') count++;
    }
    return count;
}

inline size_t intersect_sorted_raw(const uint32_t* A, size_t nA,
                                   const uint32_t* B, size_t nB,
                                   uint32_t* dst) {
    size_t i = 0, j = 0, k = 0;
    while (i < nA && j < nB) {
        uint32_t a = A[i], b = B[j];
        if (a == b) {
            dst[k++] = a;
            i++; j++;
        } else if (a < b) {
            i++;
        } else {
            j++;
        }
    }
    return k;
}

inline size_t union_sorted_raw(const uint32_t* A, size_t nA,
                               const uint32_t* B, size_t nB,
                               uint32_t* dst) {
    size_t i = 0, j = 0, k = 0;
    while (i < nA && j < nB) {
        uint32_t a = A[i], b = B[j];
        if (a == b) {
            dst[k++] = a;
            i++; j++;
        } else if (a < b) {
            dst[k++] = a;
            i++;
        } else {
            dst[k++] = b;
            j++;
        }
    }
    while (i < nA) dst[k++] = A[i++];
    while (j < nB) dst[k++] = B[j++];
    return k;
}

inline size_t difference_sorted_raw(const uint32_t* A, size_t nA,
                                    const uint32_t* B, size_t nB,
                                    uint32_t* dst) {
    size_t i = 0, j = 0, k = 0;
    while (i < nA && j < nB) {
        uint32_t a = A[i], b = B[j];
        if (a == b) {
            i++; j++;
        } else if (a < b) {
            dst[k++] = a;
            i++;
        } else {
            j++;
        }
    }
    while (i < nA) dst[k++] = A[i++];
    return k;
}

struct PostingTuple {
    uint32_t token_id;
    uint32_t doc_id;
};

inline bool match_phrase_streaming(const char* p, const char* end,
                                   const std::vector<std::string_view>& terms) {
    size_t k = terms.size();
    if (k == 0) return true;

    size_t match_idx = 0;
    while (p < end) {
        while (p < end && TOKEN_CHAR[(uint8_t)*p] == 0) p++;
        if (p >= end) break;
        const char* tok_s = p;
        while (p < end && TOKEN_CHAR[(uint8_t)*p] != 0) p++;
        size_t tok_len = p - tok_s;

        std::string_view expected = terms[match_idx];
        bool matches = (tok_len == expected.size());
        if (matches) {
            for (size_t m = 0; m < tok_len; ++m) {
                if (TOKEN_CHAR[(uint8_t)tok_s[m]] != (uint8_t)expected[m]) {
                    matches = false;
                    break;
                }
            }
        }

        if (matches) {
            match_idx++;
            if (match_idx == k) return true;
        } else if (match_idx > 0) {
            expected = terms[0];
            bool matches_first = (tok_len == expected.size());
            if (matches_first) {
                for (size_t m = 0; m < tok_len; ++m) {
                    if (TOKEN_CHAR[(uint8_t)tok_s[m]] != (uint8_t)expected[m]) {
                        matches_first = false;
                        break;
                    }
                }
            }
            match_idx = matches_first ? 1 : 0;
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    std::string docs_path = data_dir + "/docs.txt";
    std::string queries_path = data_dir + "/queries.txt";
    std::string results_path = out_dir + "/results.txt";

    ferocious::MappedFile docs_mf(docs_path.c_str());
    if (!docs_mf.ok) return 1;

    ferocious::MappedFile queries_mf(queries_path.c_str());
    if (!queries_mf.ok) return 1;

    const char* docs_data = static_cast<const char*>(docs_mf.data);
    size_t docs_size = docs_mf.size;

    const char* queries_data = static_cast<const char*>(queries_mf.data);
    size_t queries_size = queries_mf.size;

    unsigned num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    // 1. Scan queries to extract active vocabulary
    std::vector<std::string_view> query_lines;
    uint32_t num_active_tokens = 0;

    auto insert_token = [&](std::string_view sv) -> uint32_t {
        uint64_t h = fnv1a_str(sv);
        size_t slot = h & VOCAB_MASK;
        while (vocab_table[slot].hash != 0) {
            if (vocab_table[slot].hash == h) return vocab_table[slot].token_id;
            slot = (slot + 1) & VOCAB_MASK;
        }
        uint32_t new_id = num_active_tokens++;
        vocab_table[slot].hash = h;
        vocab_table[slot].token_id = new_id;
        return new_id;
    };

    auto find_token = [&](std::string_view sv) -> int64_t {
        uint64_t h = fnv1a_str(sv);
        size_t slot = h & VOCAB_MASK;
        while (vocab_table[slot].hash != 0) {
            if (vocab_table[slot].hash == h) return vocab_table[slot].token_id;
            slot = (slot + 1) & VOCAB_MASK;
        }
        return -1;
    };

    {
        const char* p = queries_data;
        const char* end = queries_data + queries_size;
        while (p < end) {
            const char* qend = static_cast<const char*>(std::memchr(p, '\n', end - p));
            if (!qend) qend = end;
            const char* next_p = (qend < end) ? qend + 1 : end;
            while (qend > p && (qend[-1] == '\r' || qend[-1] == ' ' || qend[-1] == '\t')) qend--;

            if (qend > p) {
                std::string_view qline(p, qend - p);
                query_lines.push_back(qline);

                if (qline.starts_with("BOOL ")) {
                    std::string_view rest = qline.substr(5);
                    size_t pos = 0;
                    while (pos < rest.size()) {
                        size_t next_sp = rest.find(' ', pos);
                        std::string_view part = (next_sp == std::string_view::npos) ? rest.substr(pos) : rest.substr(pos, next_sp - pos);
                        if (part != "AND" && part != "OR" && part != "NOT") {
                            insert_token(part);
                        }
                        if (next_sp == std::string_view::npos) break;
                        pos = next_sp + 1;
                    }
                } else if (qline.starts_with("PHRASE \"")) {
                    std::string_view inner = qline.substr(8);
                    if (!inner.empty() && inner.back() == '"') inner.remove_suffix(1);
                    size_t pos = 0;
                    while (pos < inner.size()) {
                        size_t next_sp = inner.find(' ', pos);
                        std::string_view part = (next_sp == std::string_view::npos) ? inner.substr(pos) : inner.substr(pos, next_sp - pos);
                        if (!part.empty()) {
                            insert_token(part);
                        }
                        if (next_sp == std::string_view::npos) break;
                        pos = next_sp + 1;
                    }
                }
            }
            p = next_p;
        }
    }

    // 2. Parallel SIMD chunk partition
    std::vector<const char*> chunk_starts(num_threads);
    std::vector<const char*> chunk_ends(num_threads);
    size_t chunk_bytes = docs_size / num_threads;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        const char* s = docs_data + tid * chunk_bytes;
        const char* e = (tid == num_threads - 1) ? docs_data + docs_size : docs_data + (tid + 1) * chunk_bytes;
        if (tid > 0) { while (s < docs_data + docs_size && *s != '\n') s++; if (s < docs_data + docs_size) s++; }
        if (tid < num_threads - 1) { while (e < docs_data + docs_size && *e != '\n') e++; if (e < docs_data + docs_size) e++; }
        chunk_starts[tid] = s;
        chunk_ends[tid] = e;
    }

    std::vector<size_t> thread_doc_counts(num_threads, 0);
    std::vector<std::thread> workers;
    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            if (chunk_starts[tid] < chunk_ends[tid]) {
                thread_doc_counts[tid] = count_newlines_simd(chunk_starts[tid], chunk_ends[tid] - chunk_starts[tid]);
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    std::vector<uint32_t> thread_start_doc_id(num_threads, 0);
    for (unsigned tid = 1; tid < num_threads; ++tid) {
        thread_start_doc_id[tid] = thread_start_doc_id[tid - 1] + thread_doc_counts[tid - 1];
    }
    size_t total_docs = thread_start_doc_id.back() + thread_doc_counts.back();

    struct DocSpan {
        const char* start = nullptr;
        const char* end = nullptr;
    };
    std::vector<DocSpan> doc_spans(total_docs);

    // 3. 1-Pass Flat Token Ingest
    std::vector<std::vector<PostingTuple>> thread_tuples(num_threads);
    std::vector<std::vector<uint32_t>> thread_counts(num_threads, std::vector<uint32_t>(num_active_tokens, 0));

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* cur = chunk_starts[tid];
            const char* limit = chunk_ends[tid];
            uint32_t cur_doc_id = thread_start_doc_id[tid];
            auto& tuples = thread_tuples[tid];
            tuples.reserve(2500000);
            auto& counts = thread_counts[tid];
            std::vector<uint32_t> doc_active_ids;
            doc_active_ids.reserve(64);

            while (cur < limit) {
                const char* line_end = static_cast<const char*>(std::memchr(cur, '\n', limit - cur));
                if (!line_end) line_end = limit;
                const char* next_cur = (line_end < limit) ? line_end + 1 : limit;

                const char* text_end = line_end;
                while (text_end > cur && (text_end[-1] == '\r' || text_end[-1] == ' ' || text_end[-1] == '\t')) text_end--;

                const char* tab_pos = (text_end > cur) ? static_cast<const char*>(std::memchr(cur, '\t', text_end - cur)) : nullptr;
                const char* text_start = tab_pos ? tab_pos + 1 : cur;

                if (cur_doc_id < total_docs) {
                    doc_spans[cur_doc_id] = {text_start, text_end};
                }

                doc_active_ids.clear();
                const char* p = text_start;
                while (p < text_end) {
                    while (p < text_end && TOKEN_CHAR[(uint8_t)*p] == 0) p++;
                    if (p >= text_end) break;
                    const char* tok_s = p;
                    uint64_t h = 0xcbf29ce484222325ULL;
                    while (p < text_end) {
                        uint8_t tc = TOKEN_CHAR[(uint8_t)*p];
                        if (tc == 0) break;
                        h = (h ^ tc) * 0x100000001b3ULL;
                        p++;
                    }
                    if (h == 0) h = 1;

                    size_t slot = h & VOCAB_MASK;
                    while (vocab_table[slot].hash != 0) {
                        if (vocab_table[slot].hash == h) {
                            doc_active_ids.push_back(vocab_table[slot].token_id);
                            break;
                        }
                        slot = (slot + 1) & VOCAB_MASK;
                    }
                }

                if (!doc_active_ids.empty()) {
                    std::sort(doc_active_ids.begin(), doc_active_ids.end());
                    uint32_t prev = UINT32_MAX;
                    for (uint32_t tok_id : doc_active_ids) {
                        if (tok_id != prev) {
                            tuples.push_back({tok_id, cur_doc_id});
                            counts[tok_id]++;
                            prev = tok_id;
                        }
                    }
                }

                cur_doc_id++;
                cur = next_cur;
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();

    // 4. Prefix Sums & CSR Scatter
    std::vector<uint32_t> csr_offsets(num_active_tokens + 1, 0);
    std::vector<std::vector<uint32_t>> thread_write_ptrs(num_threads, std::vector<uint32_t>(num_active_tokens, 0));

    uint32_t running_total = 0;
    for (uint32_t tok_id = 0; tok_id < num_active_tokens; ++tok_id) {
        csr_offsets[tok_id] = running_total;
        uint32_t t_base = running_total;
        for (unsigned tid = 0; tid < num_threads; ++tid) {
            thread_write_ptrs[tid][tok_id] = t_base;
            t_base += thread_counts[tid][tok_id];
        }
        running_total = t_base;
    }
    csr_offsets[num_active_tokens] = running_total;
    uint32_t total_postings = running_total;

    std::vector<uint32_t> csr_doc_ids(total_postings);

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            auto& write_ptrs = thread_write_ptrs[tid];
            for (const auto& tup : thread_tuples[tid]) {
                uint32_t pos = write_ptrs[tup.token_id]++;
                csr_doc_ids[pos] = tup.doc_id;
            }
            std::vector<PostingTuple>().swap(thread_tuples[tid]);
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();
    thread_tuples.clear();

    // 5. Query execution with streaming phrase verification
    size_t total_queries = query_lines.size();
    std::vector<std::string> thread_outputs(num_threads);
    size_t q_chunk = (total_queries + num_threads - 1) / num_threads;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            size_t q_start = tid * q_chunk;
            size_t q_end = std::min(total_queries, q_start + q_chunk);
            if (q_start >= q_end) return;

            std::string& out_str = thread_outputs[tid];
            out_str.reserve((q_end - q_start) * 32);

            std::vector<uint32_t> buf_a(262144);
            std::vector<uint32_t> buf_b(262144);
            std::vector<uint32_t> matches;
            matches.reserve(4096);

            for (size_t q = q_start; q < q_end; ++q) {
                std::string_view qline = query_lines[q];

                if (qline.starts_with("BOOL ")) {
                    std::string_view rest = qline.substr(5);
                    std::vector<std::string_view> parts;
                    parts.reserve(16);
                    size_t pos = 0;
                    while (pos < rest.size()) {
                        size_t next_sp = rest.find(' ', pos);
                        if (next_sp == std::string_view::npos) {
                            parts.push_back(rest.substr(pos));
                            break;
                        }
                        parts.push_back(rest.substr(pos, next_sp - pos));
                        pos = next_sp + 1;
                    }

                    if (parts.empty()) {
                        out_str.append("0 0\n");
                        continue;
                    }

                    int64_t id0 = find_token(parts[0]);
                    uint32_t* cur_buf = buf_a.data();
                    uint32_t* next_buf = buf_b.data();
                    size_t cur_len = 0;

                    if (id0 >= 0 && (size_t)id0 < num_active_tokens) {
                        uint32_t start = csr_offsets[id0];
                        uint32_t end = csr_offsets[id0 + 1];
                        cur_len = end - start;
                        if (cur_len > 0) {
                            std::memcpy(cur_buf, csr_doc_ids.data() + start, cur_len * sizeof(uint32_t));
                        }
                    }

                    for (size_t i = 1; i + 1 < parts.size(); i += 2) {
                        std::string_view op = parts[i];
                        std::string_view term = parts[i + 1];

                        int64_t post_id = find_token(term);
                        const uint32_t* post_data = nullptr;
                        size_t post_len = 0;
                        if (post_id >= 0 && (size_t)post_id < num_active_tokens) {
                            uint32_t start = csr_offsets[post_id];
                            uint32_t end = csr_offsets[post_id + 1];
                            post_len = end - start;
                            post_data = csr_doc_ids.data() + start;
                        }

                        size_t next_len = 0;
                        if (op == "AND") {
                            if (post_len > 0 && cur_len > 0) {
                                next_len = intersect_sorted_raw(cur_buf, cur_len, post_data, post_len, next_buf);
                            }
                            cur_len = next_len;
                            std::swap(cur_buf, next_buf);
                        } else if (op == "OR") {
                            if (post_len > 0) {
                                next_len = union_sorted_raw(cur_buf, cur_len, post_data, post_len, next_buf);
                                cur_len = next_len;
                                std::swap(cur_buf, next_buf);
                            }
                        } else if (op == "NOT") {
                            if (post_len > 0 && cur_len > 0) {
                                next_len = difference_sorted_raw(cur_buf, cur_len, post_data, post_len, next_buf);
                                cur_len = next_len;
                                std::swap(cur_buf, next_buf);
                            }
                        }
                    }

                    size_t count = cur_len;
                    uint64_t sum_ids = 0;
                    for (size_t k = 0; k < count; ++k) sum_ids += cur_buf[k];

                    append_u64(out_str, count);
                    out_str.push_back(' ');
                    append_u64(out_str, sum_ids);

                    size_t top_m = std::min(count, size_t(10));
                    for (size_t k = 0; k < top_m; ++k) {
                        out_str.push_back(' ');
                        append_u32(out_str, cur_buf[k]);
                    }
                    out_str.push_back('\n');

                } else if (qline.starts_with("PHRASE \"")) {
                    std::string_view inner = qline.substr(8);
                    if (!inner.empty() && inner.back() == '"') {
                        inner.remove_suffix(1);
                    }

                    std::vector<std::string_view> phrase_terms;
                    size_t pos = 0;
                    while (pos < inner.size()) {
                        size_t next_sp = inner.find(' ', pos);
                        if (next_sp == std::string_view::npos) {
                            phrase_terms.emplace_back(inner.substr(pos));
                            break;
                        }
                        phrase_terms.emplace_back(inner.substr(pos, next_sp - pos));
                        pos = next_sp + 1;
                    }

                    if (phrase_terms.empty()) {
                        out_str.append("0 0\n");
                        continue;
                    }

                    int64_t id0 = find_token(phrase_terms[0]);
                    uint32_t* cur_cand = buf_a.data();
                    uint32_t* next_cand = buf_b.data();
                    size_t cur_cand_len = 0;

                    if (id0 >= 0 && (size_t)id0 < num_active_tokens) {
                        uint32_t start = csr_offsets[id0];
                        uint32_t end = csr_offsets[id0 + 1];
                        cur_cand_len = end - start;
                        if (cur_cand_len > 0) {
                            std::memcpy(cur_cand, csr_doc_ids.data() + start, cur_cand_len * sizeof(uint32_t));
                        }
                    }

                    bool possible = (cur_cand_len > 0);
                    for (size_t i = 1; i < phrase_terms.size() && possible; ++i) {
                        int64_t post_id = find_token(phrase_terms[i]);
                        if (post_id < 0 || (size_t)post_id >= num_active_tokens) {
                            cur_cand_len = 0;
                            possible = false;
                            break;
                        }
                        uint32_t start = csr_offsets[post_id];
                        uint32_t end = csr_offsets[post_id + 1];
                        size_t post_len = end - start;
                        if (post_len == 0) {
                            cur_cand_len = 0;
                            possible = false;
                            break;
                        }
                        size_t next_len = intersect_sorted_raw(cur_cand, cur_cand_len,
                                                               csr_doc_ids.data() + start, post_len,
                                                               next_cand);
                        cur_cand_len = next_len;
                        std::swap(cur_cand, next_cand);
                        if (cur_cand_len == 0) possible = false;
                    }

                    matches.clear();
                    for (size_t c = 0; c < cur_cand_len; ++c) {
                        uint32_t doc_id = cur_cand[c];
                        if (doc_id >= doc_spans.size()) continue;
                        const auto& span = doc_spans[doc_id];
                        if (span.start == nullptr || span.end <= span.start) continue;

                        if (match_phrase_streaming(span.start, span.end, phrase_terms)) {
                            matches.push_back(doc_id);
                        }
                    }

                    size_t count = matches.size();
                    uint64_t sum_ids = 0;
                    for (uint32_t id : matches) sum_ids += id;

                    append_u64(out_str, count);
                    out_str.push_back(' ');
                    append_u64(out_str, sum_ids);

                    size_t top_m = std::min(count, size_t(10));
                    for (size_t m = 0; m < top_m; ++m) {
                        out_str.push_back(' ');
                        append_u32(out_str, matches[m]);
                    }
                    out_str.push_back('\n');
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(results_path.c_str(), "wb");
    if (!fp) return 1;

    ferocious::OutBuf out(fp);
    for (const auto& s : thread_outputs) {
        out.puts(s.data(), s.size());
    }
    out.flush();
    fclose(fp);

    return 0;
}
