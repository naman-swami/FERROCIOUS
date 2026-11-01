// FERROCIOUS Track B — Ultra-Fast SIMD Chunked Flat Hash Inverted Index Engine
// Zero External Dependencies (D = 1.00). C++20.

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <thread>
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

// Fast SIMD newline counting
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

    // 1. Parse all queries to extract query lines and active vocabulary
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

    // 2. Parallel SIMD chunk partition of docs.txt
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

    // 3. Ultra-Fast Parallel Register-Hashing Tokenization across 12 threads
    std::vector<std::vector<std::vector<uint32_t>>> thread_indexes(num_threads);
    for (unsigned tid = 0; tid < num_threads; ++tid) {
        thread_indexes[tid].resize(num_active_tokens);
    }

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* cur = chunk_starts[tid];
            const char* limit = chunk_ends[tid];
            uint32_t cur_doc_id = thread_start_doc_id[tid];
            auto& local_index = thread_indexes[tid];
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
                doc_spans[cur_doc_id] = {text_start, text_end};

                doc_active_ids.clear();
                const char* dp = text_start;
                const char* dend = text_end;

                while (dp < dend) {
                    while (dp < dend && TOKEN_CHAR[(uint8_t)*dp] == 0) dp++;
                    if (dp >= dend) break;
                    uint64_t h = 0xcbf29ce484222325ULL;
                    while (dp < dend) {
                        uint8_t tc = TOKEN_CHAR[(uint8_t)*dp];
                        if (tc == 0) break;
                        h = (h ^ tc) * 0x100000001b3ULL;
                        dp++;
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
                            local_index[tok_id].push_back(cur_doc_id);
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

    // 4. 12-thread Parallel Index Merge
    std::vector<std::vector<uint32_t>> global_index(num_active_tokens);
    size_t tokens_per_thread = (num_active_tokens + num_threads - 1) / num_threads;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            size_t tok_start = tid * tokens_per_thread;
            size_t tok_end = std::min((size_t)num_active_tokens, tok_start + tokens_per_thread);
            for (size_t tok_id = tok_start; tok_id < tok_end; ++tok_id) {
                size_t total_p = 0;
                for (unsigned t = 0; t < num_threads; ++t) total_p += thread_indexes[t][tok_id].size();
                global_index[tok_id].reserve(total_p);
                for (unsigned t = 0; t < num_threads; ++t) {
                    const auto& pt = thread_indexes[t][tok_id];
                    global_index[tok_id].insert(global_index[tok_id].end(), pt.begin(), pt.end());
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    workers.clear();
    thread_indexes.clear();

    // 5. Parallel Query Execution across 12 hardware threads
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

            std::vector<char> phrase_buf;
            phrase_buf.reserve(4096);
            std::vector<std::string_view> doc_toks;
            doc_toks.reserve(512);
            std::vector<uint32_t> buf_a;
            std::vector<uint32_t> buf_b;
            buf_a.reserve(32768);
            buf_b.reserve(32768);
            std::vector<uint32_t> matches;
            matches.reserve(4096);

            for (size_t qi = q_start; qi < q_end; ++qi) {
                const auto& qline = query_lines[qi];

                if (qline.starts_with("BOOL ")) {
                    std::string_view rest = qline.substr(5);
                    std::vector<std::string_view> parts;
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
                    std::vector<uint32_t>* cur_buf = &buf_a;
                    std::vector<uint32_t>* next_buf = &buf_b;
                    cur_buf->clear();

                    if (id0 >= 0 && (size_t)id0 < global_index.size()) {
                        *cur_buf = global_index[id0];
                    }

                    for (size_t i = 1; i + 1 < parts.size(); i += 2) {
                        std::string_view op = parts[i];
                        std::string_view term = parts[i + 1];

                        int64_t post_id = find_token(term);
                        const std::vector<uint32_t>* postings = (post_id >= 0 && (size_t)post_id < global_index.size()) ? &global_index[post_id] : nullptr;

                        next_buf->clear();
                        if (op == "AND") {
                            if (postings && !cur_buf->empty()) {
                                std::set_intersection(cur_buf->begin(), cur_buf->end(),
                                                      postings->begin(), postings->end(),
                                                      std::back_inserter(*next_buf));
                            }
                            std::swap(cur_buf, next_buf);
                        } else if (op == "OR") {
                            if (postings && !postings->empty()) {
                                std::set_union(cur_buf->begin(), cur_buf->end(),
                                               postings->begin(), postings->end(),
                                               std::back_inserter(*next_buf));
                                std::swap(cur_buf, next_buf);
                            }
                        } else if (op == "NOT") {
                            if (postings && !postings->empty() && !cur_buf->empty()) {
                                std::set_difference(cur_buf->begin(), cur_buf->end(),
                                                    postings->begin(), postings->end(),
                                                    std::back_inserter(*next_buf));
                                std::swap(cur_buf, next_buf);
                            }
                        }
                    }

                    size_t count = cur_buf->size();
                    uint64_t sum_ids = 0;
                    for (uint32_t id : *cur_buf) sum_ids += id;

                    append_u64(out_str, count);
                    out_str.push_back(' ');
                    append_u64(out_str, sum_ids);

                    size_t top_m = std::min(count, size_t(10));
                    for (size_t k = 0; k < top_m; ++k) {
                        out_str.push_back(' ');
                        append_u32(out_str, (*cur_buf)[k]);
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
                    std::vector<uint32_t>* cur_cand = &buf_a;
                    std::vector<uint32_t>* next_cand = &buf_b;
                    cur_cand->clear();

                    if (id0 >= 0 && (size_t)id0 < global_index.size()) {
                        *cur_cand = global_index[id0];
                    }

                    bool possible = !cur_cand->empty();
                    for (size_t i = 1; i < phrase_terms.size() && possible; ++i) {
                        int64_t post_id = find_token(phrase_terms[i]);
                        if (post_id < 0 || (size_t)post_id >= global_index.size()) {
                            cur_cand->clear();
                            possible = false;
                            break;
                        }
                        next_cand->clear();
                        const auto& plist = global_index[post_id];
                        std::set_intersection(cur_cand->begin(), cur_cand->end(),
                                              plist.begin(), plist.end(),
                                              std::back_inserter(*next_cand));
                        std::swap(cur_cand, next_cand);
                        if (cur_cand->empty()) possible = false;
                    }

                    size_t k = phrase_terms.size();
                    matches.clear();

                    for (uint32_t doc_id : *cur_cand) {
                        if (doc_id >= doc_spans.size()) continue;
                        const auto& span = doc_spans[doc_id];
                        if (span.start == nullptr || span.end <= span.start) continue;

                        size_t doc_len = span.end - span.start;
                        phrase_buf.resize(doc_len);
                        char* buf_p = phrase_buf.data();

                        doc_toks.clear();
                        const char* p = span.start;
                        while (p < span.end) {
                            while (p < span.end && TOKEN_CHAR[(uint8_t)*p] == 0) p++;
                            if (p >= span.end) break;
                            const char* tok_s = p;
                            size_t off = tok_s - span.start;
                            while (p < span.end && TOKEN_CHAR[(uint8_t)*p] != 0) {
                                buf_p[p - span.start] = (char)TOKEN_CHAR[(uint8_t)*p];
                                p++;
                            }
                            doc_toks.emplace_back(buf_p + off, (p - span.start) - off);
                        }

                        if (doc_toks.size() < k) continue;
                        for (size_t i = 0; i + k <= doc_toks.size(); ++i) {
                            bool match = true;
                            for (size_t j = 0; j < k; ++j) {
                                if (doc_toks[i + j] != phrase_terms[j]) {
                                    match = false;
                                    break;
                                }
                            }
                            if (match) {
                                matches.push_back(doc_id);
                                break;
                            }
                        }
                    }

                    size_t count = matches.size();
                    uint64_t sum_ids = 0;
                    for (uint32_t id : matches) sum_ids += id;

                    append_u64(out_str, count);
                    out_str.push_back(' ');
                    append_u64(out_str, sum_ids);

                    size_t top_m = std::min(count, size_t(10));
                    for (size_t m_i = 0; m_i < top_m; ++m_i) {
                        out_str.push_back(' ');
                        append_u32(out_str, matches[m_i]);
                    }
                    out_str.push_back('\n');
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    // 6. Write results.txt
    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(results_path.c_str(), "wb");
    if (!fp) return 1;

    for (const auto& s : thread_outputs) {
        fwrite(s.data(), 1, s.size(), fp);
    }
    fclose(fp);

    return 0;
}
