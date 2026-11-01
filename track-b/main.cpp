// FERROCIOUS Track B — Optimized Parallel Indexing & Query Engine
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
#include "ferocious_common.hpp"

namespace {

inline constexpr auto TOKEN_CHAR = [] {
    std::array<uint8_t, 256> t{};
    for (int c = '0'; c <= '9'; ++c) t[c] = (uint8_t)c;
    for (int c = 'a'; c <= 'z'; ++c) t[c] = (uint8_t)c;
    for (int c = 'A'; c <= 'Z'; ++c) t[c] = (uint8_t)(c + 32);
    return t;
}();

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return ferocious::hash64(sv.data(), sv.size());
    }
};

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
    std::unordered_map<std::string, uint16_t, TransparentStringHash, std::equal_to<>> token_to_id;
    token_to_id.reserve(32768);

    auto get_or_add_token = [&](std::string_view sv) -> uint16_t {
        auto it = token_to_id.find(sv);
        if (it != token_to_id.end()) return it->second;
        uint16_t new_id = static_cast<uint16_t>(token_to_id.size());
        token_to_id.emplace(std::string(sv), new_id);
        return new_id;
    };

    {
        const char* p = queries_data;
        const char* end = queries_data + queries_size;
        while (p < end) {
            const char* qend = static_cast<const char*>(std::memchr(p, '\n', end - p));
            if (!qend) qend = end;
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
                            get_or_add_token(part);
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
                            get_or_add_token(part);
                        }
                        if (next_sp == std::string_view::npos) break;
                        pos = next_sp + 1;
                    }
                }
            }
            p = (qend < end) ? qend + 1 : end;
        }
    }

    uint16_t num_active_tokens = static_cast<uint16_t>(token_to_id.size());

    // 2. Pre-scan docs to find doc boundaries and line pointers
    struct DocSpan {
        const char* start = nullptr;
        const char* end = nullptr;
    };
    std::vector<DocSpan> doc_spans;
    doc_spans.reserve(300000);

    {
        const char* cur = docs_data;
        const char* docs_end = docs_data + docs_size;
        while (cur < docs_end) {
            const char* line_end = static_cast<const char*>(std::memchr(cur, '\n', docs_end - cur));
            if (!line_end) line_end = docs_end;

            const char* tab_pos = static_cast<const char*>(std::memchr(cur, '\t', line_end - cur));
            if (tab_pos) {
                doc_spans.push_back({tab_pos + 1, line_end});
            }
            cur = (line_end < docs_end) ? line_end + 1 : docs_end;
        }
    }

    size_t total_docs = doc_spans.size();

    // 3. Parallel Tokenization and Posting Indexing
    // Each thread maintains a vector of vector<uint32_t> for each active token
    std::vector<std::vector<std::vector<uint32_t>>> thread_indexes(num_threads);
    for (unsigned tid = 0; tid < num_threads; ++tid) {
        thread_indexes[tid].resize(num_active_tokens);
    }

    size_t docs_per_thread = (total_docs + num_threads - 1) / num_threads;
    std::vector<std::thread> index_workers;
    index_workers.reserve(num_threads);

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        index_workers.emplace_back([&, tid]() {
            size_t start_id = tid * docs_per_thread;
            size_t end_id = std::min(total_docs, start_id + docs_per_thread);
            if (start_id >= end_id) return;

            auto& local_index = thread_indexes[tid];
            std::string lower_token;
            lower_token.reserve(128);
            std::vector<uint16_t> doc_active_ids;
            doc_active_ids.reserve(64);

            for (size_t doc_id = start_id; doc_id < end_id; ++doc_id) {
                const auto& span = doc_spans[doc_id];
                const char* p = span.start;
                const char* text_end = span.end;

                doc_active_ids.clear();
                while (p < text_end) {
                    while (p < text_end && TOKEN_CHAR[(uint8_t)*p] == 0) p++;
                    if (p >= text_end) break;
                    lower_token.clear();
                    while (p < text_end && TOKEN_CHAR[(uint8_t)*p] != 0) {
                        lower_token.push_back((char)TOKEN_CHAR[(uint8_t)*p]);
                        p++;
                    }
                    auto it = token_to_id.find(std::string_view(lower_token));
                    if (it != token_to_id.end()) {
                        doc_active_ids.push_back(it->second);
                    }
                }

                if (!doc_active_ids.empty()) {
                    std::sort(doc_active_ids.begin(), doc_active_ids.end());
                    doc_active_ids.erase(std::unique(doc_active_ids.begin(), doc_active_ids.end()), doc_active_ids.end());

                    for (uint16_t tok_id : doc_active_ids) {
                        local_index[tok_id].push_back(static_cast<uint32_t>(doc_id));
                    }
                }
            }
        });
    }

    for (auto& w : index_workers) w.join();

    // 4. Merge thread posting lists into global inverted index
    std::vector<std::vector<uint32_t>> global_index(num_active_tokens);
    for (uint16_t tok_id = 0; tok_id < num_active_tokens; ++tok_id) {
        size_t total_p = 0;
        for (unsigned tid = 0; tid < num_threads; ++tid) {
            total_p += thread_indexes[tid][tok_id].size();
        }
        global_index[tok_id].reserve(total_p);
        for (unsigned tid = 0; tid < num_threads; ++tid) {
            const auto& pt = thread_indexes[tid][tok_id];
            global_index[tok_id].insert(global_index[tok_id].end(), pt.begin(), pt.end());
        }
    }
    // Free thread indexes
    thread_indexes.clear();
    thread_indexes.shrink_to_fit();

    // 5. Parallel Query Execution across 12 hardware threads
    size_t total_queries = query_lines.size();
    std::vector<std::string> thread_outputs(num_threads);
    std::vector<std::thread> query_workers;
    query_workers.reserve(num_threads);

    size_t q_chunk = (total_queries + num_threads - 1) / num_threads;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        query_workers.emplace_back([&, tid]() {
            size_t q_start = tid * q_chunk;
            size_t q_end = std::min(total_queries, q_start + q_chunk);
            if (q_start >= q_end) return;

            std::string& out_str = thread_outputs[tid];
            out_str.reserve((q_end - q_start) * 32);

            std::vector<char> phrase_buf;
            std::vector<uint32_t> buf_a;
            std::vector<uint32_t> buf_b;
            buf_a.reserve(32768);
            buf_b.reserve(32768);

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

                    auto it0 = token_to_id.find(parts[0]);
                    std::vector<uint32_t>* cur_buf = &buf_a;
                    std::vector<uint32_t>* next_buf = &buf_b;
                    cur_buf->clear();

                    if (it0 != token_to_id.end()) {
                        *cur_buf = global_index[it0->second];
                    }

                    for (size_t i = 1; i + 1 < parts.size(); i += 2) {
                        std::string_view op = parts[i];
                        std::string_view term = parts[i + 1];

                        auto post_it = token_to_id.find(term);
                        const std::vector<uint32_t>* postings = (post_it != token_to_id.end()) ? &global_index[post_it->second] : nullptr;

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

                    auto it0 = token_to_id.find(phrase_terms[0]);
                    std::vector<uint32_t>* cur_cand = &buf_a;
                    std::vector<uint32_t>* next_cand = &buf_b;
                    cur_cand->clear();

                    if (it0 != token_to_id.end()) {
                        *cur_cand = global_index[it0->second];
                    }

                    bool possible = !cur_cand->empty();
                    for (size_t i = 1; i < phrase_terms.size() && possible; ++i) {
                        auto pit = token_to_id.find(phrase_terms[i]);
                        if (pit == token_to_id.end()) {
                            cur_cand->clear();
                            possible = false;
                            break;
                        }
                        next_cand->clear();
                        const auto& plist = global_index[pit->second];
                        std::set_intersection(cur_cand->begin(), cur_cand->end(),
                                              plist.begin(), plist.end(),
                                              std::back_inserter(*next_cand));
                        std::swap(cur_cand, next_cand);
                        if (cur_cand->empty()) possible = false;
                    }

                    size_t k = phrase_terms.size();
                    std::vector<uint32_t> matches;

                    for (uint32_t doc_id : *cur_cand) {
                        const auto& span = doc_spans[doc_id];
                        size_t doc_len = span.end - span.start;
                        phrase_buf.resize(doc_len);
                        char* buf_p = phrase_buf.data();

                        std::vector<std::string_view> doc_toks;
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
                            doc_toks.emplace_back(buf_p + off, p - tok_s);
                        }

                        if (doc_toks.size() < k) continue;

                        bool matched = false;
                        for (size_t i = 0; i + k <= doc_toks.size(); ++i) {
                            bool seq_ok = true;
                            for (size_t j = 0; j < k; ++j) {
                                if (doc_toks[i + j] != phrase_terms[j]) {
                                    seq_ok = false;
                                    break;
                                }
                            }
                            if (seq_ok) {
                                matched = true;
                                break;
                            }
                        }
                        if (matched) {
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
                    for (size_t i = 0; i < top_m; ++i) {
                        out_str.push_back(' ');
                        append_u32(out_str, matches[i]);
                    }
                    out_str.push_back('\n');
                }
            }
        });
    }

    for (auto& w : query_workers) w.join();

    // 6. Output writing
    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(results_path.c_str(), "wb");
    if (!fp) return 1;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        if (!thread_outputs[tid].empty()) {
            fwrite(thread_outputs[tid].data(), 1, thread_outputs[tid].size(), fp);
        }
    }
    fclose(fp);

    return 0;
}
