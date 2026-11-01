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

inline uint64_t fnv1a_str(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= (uint8_t)c;
        h *= 0x100000001b3ULL;
    }
    return (h == 0) ? 1 : h;
}

static constexpr size_t VOCAB_CAP = 262144;
static constexpr size_t VOCAB_MASK = VOCAB_CAP - 1;

struct VocabSlot {
    uint64_t hash = 0;
    uint32_t token_id = 0;
};

static VocabSlot vocab_table[VOCAB_CAP];
static uint32_t num_active_tokens = 0;

static constexpr uint32_t BITSET_WORDS = 3908;
static constexpr uint64_t TAIL_MASK = (1ULL << 16) - 1; // 250,000 % 64 = 16 bits
static constexpr uint32_t BITSET_FREQ_THRESHOLD = 4000;

inline void append_u32(std::string& s, uint32_t val) {
    char buf[16];
    char* p = buf + 16;
    if (val == 0) {
        s.push_back('0');
        return;
    }
    while (val > 0) {
        *--p = '0' + (val % 10);
        val /= 10;
    }
    s.append(p, (buf + 16) - p);
}

inline void append_u64(std::string& s, uint64_t val) {
    char buf[32];
    char* p = buf + 32;
    if (val == 0) {
        s.push_back('0');
        return;
    }
    while (val > 0) {
        *--p = '0' + (val % 10);
        val /= 10;
    }
    s.append(p, (buf + 32) - p);
}

inline size_t intersect_sorted_raw(const uint32_t* a, size_t na,
                                  const uint32_t* b, size_t nb,
                                  uint32_t* out) {
    size_t ia = 0, ib = 0, k = 0;
    while (ia < na && ib < nb) {
        uint32_t va = a[ia];
        uint32_t vb = b[ib];
        if (va == vb) {
            out[k++] = va;
            ia++;
            ib++;
        } else if (va < vb) {
            ia++;
        } else {
            ib++;
        }
    }
    return k;
}

inline size_t union_sorted_raw(const uint32_t* a, size_t na,
                              const uint32_t* b, size_t nb,
                              uint32_t* out) {
    size_t ia = 0, ib = 0, k = 0;
    while (ia < na && ib < nb) {
        uint32_t va = a[ia];
        uint32_t vb = b[ib];
        if (va == vb) {
            out[k++] = va;
            ia++;
            ib++;
        } else if (va < vb) {
            out[k++] = va;
            ia++;
        } else {
            out[k++] = vb;
            ib++;
        }
    }
    while (ia < na) out[k++] = a[ia++];
    while (ib < nb) out[k++] = b[ib++];
    return k;
}

inline size_t difference_sorted_raw(const uint32_t* a, size_t na,
                                    const uint32_t* b, size_t nb,
                                    uint32_t* out) {
    size_t ia = 0, ib = 0, k = 0;
    while (ia < na && ib < nb) {
        uint32_t va = a[ia];
        uint32_t vb = b[ib];
        if (va == vb) {
            ia++;
            ib++;
        } else if (va < vb) {
            out[k++] = va;
            ia++;
        } else {
            ib++;
        }
    }
    while (ia < na) out[k++] = a[ia++];
    return k;
}

inline bool match_phrase_streaming(const char* p, const char* end,
                                   const std::string_view* terms, size_t k) {
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
    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    std::string docs_path = data_dir + "/docs.txt";
    std::string queries_path = data_dir + "/queries.txt";
    std::string results_path = out_dir + "/results.txt";

    ferocious::MappedFile dmf(docs_path.c_str());
    if (!dmf.ok) return 1;
    const char* docs_data = static_cast<const char*>(dmf.data);
    size_t docs_size = dmf.size;

    ferocious::MappedFile qmf(queries_path.c_str());
    if (!qmf.ok) return 1;
    const char* queries_data = static_cast<const char*>(qmf.data);
    size_t queries_size = qmf.size;

    unsigned num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    // 1. Scan queries.txt to collect unique vocabulary
    std::vector<std::string_view> query_lines;
    query_lines.reserve(65536);

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

    const char* qp = queries_data;
    const char* qend = queries_data + queries_size;
    while (qp < qend) {
        const char* nl = static_cast<const char*>(std::memchr(qp, '\n', qend - qp));
        const char* line_end = nl ? nl : qend;
        size_t line_len = line_end - qp;
        if (line_len > 0 && qp[line_len - 1] == '\r') line_len--;
        std::string_view qline(qp, line_len);
        query_lines.push_back(qline);

        if (qline.starts_with("BOOL ")) {
            size_t p = 5;
            while (p < qline.size()) {
                while (p < qline.size() && qline[p] == ' ') p++;
                if (p >= qline.size()) break;
                size_t next_sp = qline.find(' ', p);
                if (next_sp == std::string_view::npos) next_sp = qline.size();
                std::string_view word = qline.substr(p, next_sp - p);
                p = next_sp;
                if (word != "AND" && word != "OR" && word != "NOT") {
                    insert_token(word);
                }
            }
        } else if (qline.starts_with("PHRASE \"")) {
            std::string_view inner = qline.substr(8);
            if (!inner.empty() && inner.back() == '"') inner.remove_suffix(1);
            size_t p = 0;
            while (p < inner.size()) {
                while (p < inner.size() && inner[p] == ' ') p++;
                if (p >= inner.size()) break;
                size_t next_sp = inner.find(' ', p);
                if (next_sp == std::string_view::npos) next_sp = inner.size();
                std::string_view word = inner.substr(p, next_sp - p);
                p = next_sp;
                insert_token(word);
            }
        }
        qp = nl ? nl + 1 : qend;
    }

    // 2. Discover document line boundaries
    size_t chunk_bytes = docs_size / num_threads;
    std::vector<const char*> chunk_starts(num_threads);
    std::vector<const char*> chunk_ends(num_threads);

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        const char* s = docs_data + tid * chunk_bytes;
        const char* e = (tid == num_threads - 1) ? (docs_data + docs_size) : (docs_data + (tid + 1) * chunk_bytes);
        if (tid > 0) { while (s < docs_data + docs_size && *s != '\n') s++; if (s < docs_data + docs_size) s++; }
        if (tid < num_threads - 1) { while (e < docs_data + docs_size && *e != '\n') e++; if (e < docs_data + docs_size) e++; }
        chunk_starts[tid] = s;
        chunk_ends[tid] = e;
    }

    std::vector<size_t> thread_doc_counts(num_threads, 0);
    std::vector<std::thread> workers;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* cur = chunk_starts[tid];
            const char* limit = chunk_ends[tid];
            size_t cnt = 0;
            while (cur < limit) {
                const char* nl = static_cast<const char*>(std::memchr(cur, '\n', limit - cur));
                cnt++;
                cur = nl ? nl + 1 : limit;
            }
            thread_doc_counts[tid] = cnt;
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

    // 3. Pass 1: Count posting frequencies per active token per thread
    std::vector<std::vector<uint32_t>> thread_counts(num_threads, std::vector<uint32_t>(num_active_tokens, 0));

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            const char* cur = chunk_starts[tid];
            const char* limit = chunk_ends[tid];
            uint32_t cur_doc_id = thread_start_doc_id[tid];
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

                if (cur_doc_id < doc_spans.size()) {
                    doc_spans[cur_doc_id] = {text_start, text_end};
                }

                doc_active_ids.clear();
                const char* p = text_start;
                while (p < text_end) {
                    while (p < text_end && TOKEN_CHAR[(uint8_t)*p] == 0) p++;
                    if (p >= text_end) break;
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

    // 4. Prefix Sums & Direct CSR allocation
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

    // 5. Pass 2: Direct Scatter into csr_doc_ids
    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            auto& write_ptrs = thread_write_ptrs[tid];
            const char* cur = chunk_starts[tid];
            const char* limit = chunk_ends[tid];
            uint32_t cur_doc_id = thread_start_doc_id[tid];
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

                doc_active_ids.clear();
                const char* p = text_start;
                while (p < text_end) {
                    while (p < text_end && TOKEN_CHAR[(uint8_t)*p] == 0) p++;
                    if (p >= text_end) break;
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
                            uint32_t pos = write_ptrs[tok_id]++;
                            csr_doc_ids[pos] = cur_doc_id;
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

    // 6. Precompute Dense Bitsets for Frequent Terms (df >= BITSET_FREQ_THRESHOLD)
    std::vector<const uint64_t*> term_bitsets(num_active_tokens, nullptr);
    std::vector<std::vector<uint64_t>> bitset_arena;
    bitset_arena.reserve(1024);

    for (uint32_t tok_id = 0; tok_id < num_active_tokens; ++tok_id) {
        uint32_t start = csr_offsets[tok_id];
        uint32_t end = csr_offsets[tok_id + 1];
        uint32_t df = end - start;
        if (df >= BITSET_FREQ_THRESHOLD) {
            bitset_arena.emplace_back(BITSET_WORDS, 0ULL);
            uint64_t* bits = bitset_arena.back().data();
            for (uint32_t k = start; k < end; ++k) {
                uint32_t doc = csr_doc_ids[k];
                bits[doc >> 6] |= (1ULL << (doc & 63));
            }
            bits[3906] &= TAIL_MASK;
            bits[3907] = 0;
            term_bitsets[tok_id] = bits;
        }
    }

    // 7. Parallel Query Evaluation
    std::vector<std::string> thread_outputs(num_threads);
    size_t num_queries = query_lines.size();
    size_t queries_per_thread = (num_queries + num_threads - 1) / num_threads;

    for (unsigned tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            size_t q_start = tid * queries_per_thread;
            size_t q_end = std::min(num_queries, (tid + 1) * queries_per_thread);
            if (q_start >= q_end) return;

            std::string& out_str = thread_outputs[tid];
            out_str.reserve(16 * 1024 * 1024);

            std::vector<uint32_t> buf_a(262144);
            std::vector<uint32_t> buf_b(262144);
            std::vector<uint32_t> matches;
            matches.reserve(65536);

            std::string_view parts[32];
            std::string_view phrase_terms[32];

            for (size_t qidx = q_start; qidx < q_end; ++qidx) {
                std::string_view qline = query_lines[qidx];

                if (qline.starts_with("BOOL ")) {
                    std::string_view rest = qline.substr(5);
                    size_t num_parts = 0;
                    size_t pos = 0;
                    while (pos < rest.size() && num_parts < 32) {
                        size_t next_sp = rest.find(' ', pos);
                        if (next_sp == std::string_view::npos) {
                            parts[num_parts++] = rest.substr(pos);
                            break;
                        }
                        parts[num_parts++] = rest.substr(pos, next_sp - pos);
                        pos = next_sp + 1;
                    }

                    if (num_parts == 0) {
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

                    for (size_t i = 1; i + 1 < num_parts; i += 2) {
                        std::string_view op = parts[i];
                        std::string_view term = parts[i + 1];

                        int64_t post_id = find_token(term);
                        const uint32_t* post_data = nullptr;
                        size_t post_len = 0;
                        const uint64_t* post_bitset = nullptr;

                        if (post_id >= 0 && (size_t)post_id < num_active_tokens) {
                            uint32_t start = csr_offsets[post_id];
                            uint32_t end = csr_offsets[post_id + 1];
                            post_len = end - start;
                            post_data = csr_doc_ids.data() + start;
                            post_bitset = term_bitsets[post_id];
                        }

                        size_t next_len = 0;
                        if (op == "AND") {
                            if (cur_len > 0 && post_len > 0) {
                                if (post_bitset) {
                                    // Bitset Probe O(survivors)
                                    for (size_t k = 0; k < cur_len; ++k) {
                                        uint32_t doc = cur_buf[k];
                                        if ((post_bitset[doc >> 6] & (1ULL << (doc & 63))) != 0) {
                                            next_buf[next_len++] = doc;
                                        }
                                    }
                                } else {
                                    next_len = intersect_sorted_raw(cur_buf, cur_len, post_data, post_len, next_buf);
                                }
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
                            if (cur_len > 0 && post_len > 0) {
                                if (post_bitset) {
                                    // Bitset Probe O(survivors)
                                    for (size_t k = 0; k < cur_len; ++k) {
                                        uint32_t doc = cur_buf[k];
                                        if ((post_bitset[doc >> 6] & (1ULL << (doc & 63))) == 0) {
                                            next_buf[next_len++] = doc;
                                        }
                                    }
                                } else {
                                    next_len = difference_sorted_raw(cur_buf, cur_len, post_data, post_len, next_buf);
                                }
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

                    size_t num_phrase_terms = 0;
                    size_t pos = 0;
                    while (pos < inner.size() && num_phrase_terms < 32) {
                        size_t next_sp = inner.find(' ', pos);
                        if (next_sp == std::string_view::npos) {
                            phrase_terms[num_phrase_terms++] = inner.substr(pos);
                            break;
                        }
                        phrase_terms[num_phrase_terms++] = inner.substr(pos, next_sp - pos);
                        pos = next_sp + 1;
                    }

                    if (num_phrase_terms == 0) {
                        out_str.append("0 0\n");
                        continue;
                    }

                    // Find token IDs and lengths for phrase terms
                    struct PhraseTermInfo {
                        int64_t tok_id = -1;
                        uint32_t len = 0;
                        uint32_t orig_idx = 0;
                    };
                    PhraseTermInfo pinfo[32];
                    bool possible = true;

                    for (size_t i = 0; i < num_phrase_terms; ++i) {
                        int64_t tid = find_token(phrase_terms[i]);
                        pinfo[i].tok_id = tid;
                        pinfo[i].orig_idx = (uint32_t)i;
                        if (tid < 0 || (size_t)tid >= num_active_tokens) {
                            pinfo[i].len = 0;
                            possible = false;
                        } else {
                            pinfo[i].len = csr_offsets[tid + 1] - csr_offsets[tid];
                            if (pinfo[i].len == 0) possible = false;
                        }
                    }

                    if (!possible) {
                        out_str.append("0 0\n");
                        continue;
                    }

                    // Sort phrase terms by posting list length (rarest first!)
                    std::sort(pinfo, pinfo + num_phrase_terms, [](const auto& a, const auto& b) {
                        return a.len < b.len;
                    });

                    // Start candidates from the rarest term
                    uint32_t* cur_cand = buf_a.data();
                    uint32_t* next_cand = buf_b.data();
                    size_t cur_cand_len = 0;

                    int64_t id0 = pinfo[0].tok_id;
                    uint32_t start0 = csr_offsets[id0];
                    uint32_t end0 = csr_offsets[id0 + 1];
                    cur_cand_len = end0 - start0;
                    if (cur_cand_len > 0) {
                        std::memcpy(cur_cand, csr_doc_ids.data() + start0, cur_cand_len * sizeof(uint32_t));
                    }

                    // Filter candidate docs against remaining phrase terms
                    for (size_t i = 1; i < num_phrase_terms && cur_cand_len > 0; ++i) {
                        int64_t post_id = pinfo[i].tok_id;
                        const uint64_t* bits = term_bitsets[post_id];
                        size_t next_len = 0;

                        if (bits) {
                            // Instant O(candidates) bit test!
                            for (size_t c = 0; c < cur_cand_len; ++c) {
                                uint32_t doc = cur_cand[c];
                                if ((bits[doc >> 6] & (1ULL << (doc & 63))) != 0) {
                                    next_cand[next_len++] = doc;
                                }
                            }
                        } else {
                            uint32_t start = csr_offsets[post_id];
                            uint32_t post_len = csr_offsets[post_id + 1] - start;
                            next_len = intersect_sorted_raw(cur_cand, cur_cand_len,
                                                           csr_doc_ids.data() + start, post_len,
                                                           next_cand);
                        }
                        cur_cand_len = next_len;
                        std::swap(cur_cand, next_cand);
                    }

                    matches.clear();
                    for (size_t c = 0; c < cur_cand_len; ++c) {
                        uint32_t doc_id = cur_cand[c];
                        if (doc_id >= doc_spans.size()) continue;
                        const auto& span = doc_spans[doc_id];
                        if (span.start == nullptr || span.end <= span.start) continue;

                        if (match_phrase_streaming(span.start, span.end, phrase_terms, num_phrase_terms)) {
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
    for (unsigned tid = 0; tid < num_threads; ++tid) {
        out.puts(thread_outputs[tid].data(), thread_outputs[tid].size());
    }
    out.flush();
    fclose(fp);

    return 0;
}
