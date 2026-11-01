// FERROCIOUS Track B — 12-Thread Parallel Docs Indexing + Parallel Query Engine
// Zero External Dependencies (D = 1.00). C++20.

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> active_vocab;
    active_vocab.reserve(65536);

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
                            active_vocab.emplace(part);
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
                            active_vocab.emplace(part);
                        }
                        if (next_sp == std::string_view::npos) break;
                        pos = next_sp + 1;
                    }
                }
            }
            p = (qend < end) ? qend + 1 : end;
        }
    }

    // 2. Pre-scan docs to find doc boundaries and line pointers
    // docs.txt: <doc_id>\t<text>\n with dense sequential IDs starting at 0
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

    // 3. Parallel Tokenization and Inverted Index Building across 12 threads!
    // Each thread indexes a contiguous chunk of doc IDs [start_doc, end_doc)
    using PostingsMap = std::unordered_map<std::string, std::vector<uint32_t>, TransparentStringHash, std::equal_to<>>;
    std::vector<PostingsMap> thread_indexes(num_threads);

    size_t docs_per_thread = (total_docs + num_threads - 1) / num_threads;
    std::vector<std::thread> index_workers;
    index_workers.reserve(num_threads);

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        index_workers.emplace_back([&, tid]() {
            size_t start_id = tid * docs_per_thread;
            size_t end_id = std::min(total_docs, start_id + docs_per_thread);
            if (start_id >= end_id) return;

            auto& local_index = thread_indexes[tid];
            local_index.reserve(active_vocab.size());

            std::string lower_token;
            lower_token.reserve(128);
            std::vector<std::string> doc_active_terms;

            for (size_t doc_id = start_id; doc_id < end_id; ++doc_id) {
                const auto& span = doc_spans[doc_id];
                const char* p = span.start;
                const char* text_end = span.end;

                doc_active_terms.clear();
                while (p < text_end) {
                    while (p < text_end && TOKEN_CHAR[(uint8_t)*p] == 0) p++;
                    if (p >= text_end) break;
                    lower_token.clear();
                    while (p < text_end && TOKEN_CHAR[(uint8_t)*p] != 0) {
                        lower_token.push_back((char)TOKEN_CHAR[(uint8_t)*p]);
                        p++;
                    }
                    if (active_vocab.find(std::string_view(lower_token)) != active_vocab.end()) {
                        doc_active_terms.push_back(lower_token);
                    }
                }

                std::sort(doc_active_terms.begin(), doc_active_terms.end());
                doc_active_terms.erase(std::unique(doc_active_terms.begin(), doc_active_terms.end()), doc_active_terms.end());

                for (const auto& term : doc_active_terms) {
                    local_index[term].push_back(static_cast<uint32_t>(doc_id));
                }
            }
        });
    }

    for (auto& w : index_workers) w.join();

    // 4. Merge thread posting lists into global inverted index
    // Because doc IDs in thread 0 < thread 1 < ... < thread 11,
    // merging is simple ordered concatenation without any sorting!
    PostingsMap inverted_index;
    inverted_index.reserve(active_vocab.size() * 2);

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        for (auto& [term, postings] : thread_indexes[tid]) {
            auto& global_postings = inverted_index[term];
            global_postings.insert(global_postings.end(), postings.begin(), postings.end());
        }
    }

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

                    auto it = inverted_index.find(parts[0]);
                    std::vector<uint32_t> running = (it != inverted_index.end()) ? it->second : std::vector<uint32_t>{};

                    for (size_t i = 1; i + 1 < parts.size(); i += 2) {
                        std::string_view op = parts[i];
                        std::string_view term = parts[i + 1];

                        auto post_it = inverted_index.find(term);
                        const std::vector<uint32_t>* postings = (post_it != inverted_index.end()) ? &post_it->second : nullptr;

                        if (op == "AND") {
                            if (!postings || running.empty()) {
                                running.clear();
                            } else {
                                std::vector<uint32_t> next_res;
                                std::set_intersection(running.begin(), running.end(),
                                                      postings->begin(), postings->end(),
                                                      std::back_inserter(next_res));
                                running = std::move(next_res);
                            }
                        } else if (op == "OR") {
                            if (postings && !postings->empty()) {
                                std::vector<uint32_t> next_res;
                                std::set_union(running.begin(), running.end(),
                                               postings->begin(), postings->end(),
                                               std::back_inserter(next_res));
                                running = std::move(next_res);
                            }
                        } else if (op == "NOT") {
                            if (postings && !postings->empty() && !running.empty()) {
                                std::vector<uint32_t> next_res;
                                std::set_difference(running.begin(), running.end(),
                                                    postings->begin(), postings->end(),
                                                    std::back_inserter(next_res));
                                running = std::move(next_res);
                            }
                        }
                    }

                    size_t count = running.size();
                    uint64_t sum_ids = 0;
                    for (uint32_t id : running) sum_ids += id;

                    out_str.append(std::to_string(count));
                    out_str.push_back(' ');
                    out_str.append(std::to_string(sum_ids));

                    size_t top_m = std::min(count, size_t(10));
                    for (size_t k = 0; k < top_m; ++k) {
                        out_str.push_back(' ');
                        out_str.append(std::to_string(running[k]));
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

                    auto it = inverted_index.find(phrase_terms[0]);
                    std::vector<uint32_t> candidates = (it != inverted_index.end()) ? it->second : std::vector<uint32_t>{};

                    for (size_t i = 1; i < phrase_terms.size() && !candidates.empty(); ++i) {
                        auto pit = inverted_index.find(phrase_terms[i]);
                        if (pit == inverted_index.end()) {
                            candidates.clear();
                            break;
                        }
                        std::vector<uint32_t> next_cand;
                        std::set_intersection(candidates.begin(), candidates.end(),
                                              pit->second.begin(), pit->second.end(),
                                              std::back_inserter(next_cand));
                        candidates = std::move(next_cand);
                    }

                    size_t k = phrase_terms.size();
                    std::vector<uint32_t> matches;

                    for (uint32_t doc_id : candidates) {
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

                    out_str.append(std::to_string(count));
                    out_str.push_back(' ');
                    out_str.append(std::to_string(sum_ids));

                    size_t top_m = std::min(count, size_t(10));
                    for (size_t i = 0; i < top_m; ++i) {
                        out_str.push_back(' ');
                        out_str.append(std::to_string(matches[i]));
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
