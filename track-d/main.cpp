// FERROCIOUS Track D — Ultra-Low-Memory CSR + Hole-Shifting 4-ary MinHeap Bidirectional Dijkstra
// Zero External Dependencies (D = 1.00). C++20.

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <thread>
#include <atomic>
#include "ferocious_common.hpp"

namespace {

static constexpr uint64_t INF64 = 0x3FFFFFFFFFFFFFFFULL;

struct HeapItem {
    uint64_t dist;
    uint32_t u;
};

// Ultra-Fast 4-ary Min-Heap with Hole Shifting (Zero intermediate swaps)
struct MinHeap4 {
    std::vector<HeapItem> data;

    MinHeap4() {
        data.reserve(4096);
    }

    inline bool empty() const { return data.empty(); }
    inline size_t size() const { return data.size(); }
    inline void clear() { data.clear(); }
    inline uint64_t min_key() const { return data.empty() ? INF64 : data[0].dist; }

    inline void push(uint64_t dist, uint32_t u) {
        size_t i = data.size();
        data.push_back({dist, u});
        HeapItem val = {dist, u};
        while (i > 0) {
            size_t parent = (i - 1) >> 2;
            if (val.dist < data[parent].dist) {
                data[i] = data[parent];
                i = parent;
            } else {
                break;
            }
        }
        data[i] = val;
    }

    inline HeapItem pop() {
        HeapItem top = data[0];
        HeapItem val = data.back();
        data.pop_back();
        if (!data.empty()) {
            size_t i = 0;
            size_t n = data.size();
            while (true) {
                size_t child = (i << 2) + 1;
                if (child >= n) break;
                size_t best = child;
                size_t c2 = child + 1, c3 = child + 2, c4 = child + 3;
                if (c2 < n && data[c2].dist < data[best].dist) best = c2;
                if (c3 < n && data[c3].dist < data[best].dist) best = c3;
                if (c4 < n && data[c4].dist < data[best].dist) best = c4;

                if (data[best].dist < val.dist) {
                    data[i] = data[best];
                    i = best;
                } else {
                    break;
                }
            }
            data[i] = val;
        }
        return top;
    }
};

// Compact 8-byte HalfEdge for L1/L2 cache locality
struct HalfEdge {
    uint32_t to;
    uint32_t w;
};

} // namespace

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    std::string graph_path = data_dir + "/graph.txt";
    std::string queries_path = data_dir + "/queries.txt";
    std::string distances_path = out_dir + "/distances.txt";

    uint32_t n = 0, m = 0;
    std::vector<uint32_t> fwd_head;
    std::vector<uint32_t> rev_head;
    std::vector<HalfEdge> fwd_edges;
    std::vector<HalfEdge> rev_edges;

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    // 1. Scope block for graph.txt: unmaps 92 MB immediately upon CSR construction
    {
        ferocious::MappedFile gmf(graph_path.c_str());
        if (!gmf.ok) return 1;

        const char* gp = static_cast<const char*>(gmf.data);
        const char* gend = gp + gmf.size;

        while (gp < gend && (*gp == ' ' || *gp == '\t' || *gp == '\r' || *gp == '\n')) gp++;
        while (gp < gend && *gp >= '0' && *gp <= '9') n = n * 10 + (*gp++ - '0');
        while (gp < gend && (*gp == ' ' || *gp == '\t')) gp++;
        while (gp < gend && *gp >= '0' && *gp <= '9') m = m * 10 + (*gp++ - '0');
        while (gp < gend && *gp != '\n') gp++;
        if (gp < gend && *gp == '\n') gp++;

        const char* start_p = gp;
        size_t total_edge_bytes = gend - start_p;

        std::vector<const char*> chunk_starts(num_threads);
        std::vector<const char*> chunk_ends(num_threads);
        size_t chunk_bytes = total_edge_bytes / num_threads;

        for (unsigned int tid = 0; tid < num_threads; ++tid) {
            const char* s = start_p + tid * chunk_bytes;
            const char* e = (tid == num_threads - 1) ? gend : start_p + (tid + 1) * chunk_bytes;
            if (tid > 0) { while (s < gend && *s != '\n') s++; if (s < gend) s++; }
            if (tid < num_threads - 1) { while (e < gend && *e != '\n') e++; if (e < gend) e++; }
            chunk_starts[tid] = s;
            chunk_ends[tid] = e;
        }

        std::vector<std::atomic<uint32_t>> fwd_degree(n + 2);
        std::vector<std::atomic<uint32_t>> rev_degree(n + 2);
        for (uint32_t i = 0; i <= n + 1; ++i) {
            fwd_degree[i].store(0, std::memory_order_relaxed);
            rev_degree[i].store(0, std::memory_order_relaxed);
        }

        // Pass 1: Parse degrees directly into atomic counters
        std::vector<std::thread> workers;
        for (unsigned int tid = 0; tid < num_threads; ++tid) {
            workers.emplace_back([&, tid]() {
                const char* p = chunk_starts[tid];
                const char* limit = chunk_ends[tid];
                while (p < limit) {
                    while (p < limit && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
                    if (p >= limit) break;
                    uint32_t u = 0;
                    while (p < limit && *p >= '0' && *p <= '9') u = u * 10 + (*p++ - '0');
                    while (p < limit && (*p == ' ' || *p == '\t')) p++;
                    uint32_t v = 0;
                    while (p < limit && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
                    while (p < limit && *p != '\n') p++;
                    if (p < limit && *p == '\n') p++;

                    fwd_degree[u + 1].fetch_add(1, std::memory_order_relaxed);
                    rev_degree[v + 1].fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
        workers.clear();

        // Pass 2: Prefix Sums
        fwd_head.assign(n + 2, 0);
        rev_head.assign(n + 2, 0);
        std::vector<std::atomic<uint32_t>> cur_fwd(n + 2);
        std::vector<std::atomic<uint32_t>> cur_rev(n + 2);

        for (uint32_t i = 0; i <= n; ++i) {
            fwd_head[i + 1] = fwd_head[i] + fwd_degree[i + 1].load(std::memory_order_relaxed);
            rev_head[i + 1] = rev_head[i] + rev_degree[i + 1].load(std::memory_order_relaxed);
            cur_fwd[i].store(fwd_head[i], std::memory_order_relaxed);
            cur_rev[i].store(rev_head[i], std::memory_order_relaxed);
        }
        std::vector<std::atomic<uint32_t>>().swap(fwd_degree);
        std::vector<std::atomic<uint32_t>>().swap(rev_degree);

        fwd_edges.resize(m);
        rev_edges.resize(m);

        // Pass 3: Parallel Direct Scatter into compact HalfEdge arrays
        for (unsigned int tid = 0; tid < num_threads; ++tid) {
            workers.emplace_back([&, tid]() {
                const char* p = chunk_starts[tid];
                const char* limit = chunk_ends[tid];
                while (p < limit) {
                    while (p < limit && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
                    if (p >= limit) break;
                    uint32_t u = 0;
                    while (p < limit && *p >= '0' && *p <= '9') u = u * 10 + (*p++ - '0');
                    while (p < limit && (*p == ' ' || *p == '\t')) p++;
                    uint32_t v = 0;
                    while (p < limit && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
                    while (p < limit && (*p == ' ' || *p == '\t')) p++;
                    uint32_t w = 0;
                    while (p < limit && *p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
                    while (p < limit && *p != '\n') p++;
                    if (p < limit && *p == '\n') p++;

                    uint32_t fp = cur_fwd[u].fetch_add(1, std::memory_order_relaxed);
                    fwd_edges[fp] = {v, (uint32_t)w};

                    uint32_t rp = cur_rev[v].fetch_add(1, std::memory_order_relaxed);
                    rev_edges[rp] = {u, (uint32_t)w};
                }
            });
        }
        for (auto& w : workers) w.join();
    } // gmf destructor is called here! 92 MB file is completely unmapped!

    // 2. Parse queries
    std::vector<std::pair<uint32_t, uint32_t>> queries;
    {
        ferocious::MappedFile qmf(queries_path.c_str());
        if (!qmf.ok) return 1;

        const char* qp = static_cast<const char*>(qmf.data);
        const char* qend = qp + qmf.size;

        while (qp < qend) {
            while (qp < qend && (*qp == ' ' || *qp == '\t' || *qp == '\r' || *qp == '\n')) qp++;
            if (qp >= qend) break;

            uint32_t s = 0;
            while (qp < qend && *qp >= '0' && *qp <= '9') s = s * 10 + (*qp++ - '0');
            while (qp < qend && (*qp == ' ' || *qp == '\t')) qp++;

            uint32_t t = 0;
            while (qp < qend && *qp >= '0' && *qp <= '9') t = t * 10 + (*qp++ - '0');
            while (qp < qend && *qp != '\n') qp++;
            if (qp < qend && *qp == '\n') qp++;

            queries.push_back({s, t});
        }
    } // qmf unmapped here!

    size_t num_queries = queries.size();
    std::vector<int64_t> answers(num_queries, -1);
    std::atomic<size_t> query_idx{0};

    // 3. Parallel Bidirectional Dijkstra (6 dedicated P-core threads for maximum CPU throughput & low memory)
    unsigned int query_threads = std::min(num_threads, 6u);
    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < query_threads; ++tid) {
        workers.emplace_back([&]() {
            std::vector<uint64_t> f_dist(n, INF64);
            std::vector<uint64_t> r_dist(n, INF64);
            std::vector<uint32_t> f_visited;
            std::vector<uint32_t> r_visited;
            f_visited.reserve(4096);
            r_visited.reserve(4096);

            MinHeap4 f_heap;
            MinHeap4 r_heap;

            while (true) {
                size_t qid = query_idx.fetch_add(1, std::memory_order_relaxed);
                if (qid >= num_queries) break;

                uint32_t s = queries[qid].first;
                uint32_t t = queries[qid].second;

                if (s == t) {
                    answers[qid] = 0;
                    continue;
                }

                for (uint32_t u : f_visited) f_dist[u] = INF64;
                for (uint32_t u : r_visited) r_dist[u] = INF64;
                f_visited.clear();
                r_visited.clear();
                f_heap.clear();
                r_heap.clear();

                f_dist[s] = 0;
                f_visited.push_back(s);
                f_heap.push(0, s);

                r_dist[t] = 0;
                r_visited.push_back(t);
                r_heap.push(0, t);

                uint64_t mu = INF64;

                while (!f_heap.empty() && !r_heap.empty()) {
                    if (f_heap.min_key() + r_heap.min_key() >= mu) {
                        break;
                    }

                    if (f_heap.min_key() <= r_heap.min_key()) {
                        auto top = f_heap.pop();
                        uint64_t d = top.dist;
                        uint32_t u = top.u;
                        if (d > f_dist[u]) continue;

                        uint32_t e_start = fwd_head[u];
                        uint32_t e_end   = fwd_head[u + 1];
                        for (uint32_t e = e_start; e < e_end; ++e) {
                            uint32_t v = fwd_edges[e].to;
                            uint64_t nd = d + fwd_edges[e].w;
                            if (nd < f_dist[v]) {
                                if (f_dist[v] == INF64) f_visited.push_back(v);
                                f_dist[v] = nd;
                                f_heap.push(nd, v);
                                if (r_dist[v] != INF64 && nd + r_dist[v] < mu) {
                                    mu = nd + r_dist[v];
                                }
                            }
                        }
                    } else {
                        auto top = r_heap.pop();
                        uint64_t d = top.dist;
                        uint32_t u = top.u;
                        if (d > r_dist[u]) continue;

                        uint32_t e_start = rev_head[u];
                        uint32_t e_end   = rev_head[u + 1];
                        for (uint32_t e = e_start; e < e_end; ++e) {
                            uint32_t v = rev_edges[e].to;
                            uint64_t nd = d + rev_edges[e].w;
                            if (nd < r_dist[v]) {
                                if (r_dist[v] == INF64) r_visited.push_back(v);
                                r_dist[v] = nd;
                                r_heap.push(nd, v);
                                if (f_dist[v] != INF64 && nd + f_dist[v] < mu) {
                                    mu = nd + f_dist[v];
                                }
                            }
                        }
                    }
                }

                answers[qid] = (mu == INF64) ? -1 : (int64_t)mu;
            }
        });
    }

    for (auto& w : workers) w.join();

    // 4. Write distances.txt
    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(distances_path.c_str(), "wb");
    if (!fp) return 1;
    ferocious::OutBuf out(fp);

    for (size_t i = 0; i < num_queries; ++i) {
        int64_t d = answers[i];
        if (d < 0) {
            out.puts("-1\n", 3);
        } else {
            out.u64(static_cast<uint64_t>(d));
            out.put('\n');
        }
    }
    out.flush();
    fclose(fp);
    return 0;
}
