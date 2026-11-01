// FERROCIOUS Track D — 12-Thread Bidirectional 4-ary Heap Dijkstra Engine (specs/d-traverse.md)
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

// Fast 4-ary Min-Heap
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
        while (i > 0) {
            size_t parent = (i - 1) >> 2;
            if (data[i].dist < data[parent].dist) {
                std::swap(data[i], data[parent]);
                i = parent;
            } else {
                break;
            }
        }
    }

    inline HeapItem pop() {
        HeapItem top = data[0];
        HeapItem val = data.back();
        data.pop_back();
        if (!data.empty()) {
            data[0] = val;
            size_t i = 0;
            size_t n = data.size();
            while (true) {
                size_t child = (i << 2) + 1;
                if (child >= n) break;
                size_t best = child;
                size_t c2 = child + 1;
                size_t c3 = child + 2;
                size_t c4 = child + 3;
                if (c2 < n && data[c2].dist < data[best].dist) best = c2;
                if (c3 < n && data[c3].dist < data[best].dist) best = c3;
                if (c4 < n && data[c4].dist < data[best].dist) best = c4;

                if (data[best].dist < data[i].dist) {
                    std::swap(data[i], data[best]);
                    i = best;
                } else {
                    break;
                }
            }
        }
        return top;
    }
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

    ferocious::MappedFile gmf(graph_path.c_str());
    if (!gmf.ok) return 1;

    const char* gp = static_cast<const char*>(gmf.data);
    const char* gend = gp + gmf.size;

    while (gp < gend && (*gp == ' ' || *gp == '\t' || *gp == '\r' || *gp == '\n')) gp++;
    uint32_t n = 0, m = 0;
    while (gp < gend && *gp >= '0' && *gp <= '9') {
        n = n * 10 + (*gp - '0');
        gp++;
    }
    while (gp < gend && (*gp == ' ' || *gp == '\t')) gp++;
    while (gp < gend && *gp >= '0' && *gp <= '9') {
        m = m * 10 + (*gp - '0');
        gp++;
    }
    while (gp < gend && *gp != '\n') gp++;
    if (gp < gend && *gp == '\n') gp++;

    // Forward & Reverse CSR
    std::vector<uint32_t> fwd_head(n + 2, 0);
    std::vector<uint32_t> rev_head(n + 2, 0);

    struct Edge {
        uint32_t u, v;
        uint64_t w;
    };
    std::vector<Edge> edge_list;
    edge_list.reserve(m);

    while (gp < gend) {
        while (gp < gend && (*gp == ' ' || *gp == '\t' || *gp == '\r' || *gp == '\n')) gp++;
        if (gp >= gend) break;

        uint32_t u = 0;
        while (gp < gend && *gp >= '0' && *gp <= '9') {
            u = u * 10 + (*gp - '0');
            gp++;
        }
        while (gp < gend && (*gp == ' ' || *gp == '\t')) gp++;

        uint32_t v = 0;
        while (gp < gend && *gp >= '0' && *gp <= '9') {
            v = v * 10 + (*gp - '0');
            gp++;
        }
        while (gp < gend && (*gp == ' ' || *gp == '\t')) gp++;

        uint64_t w = 0;
        while (gp < gend && *gp >= '0' && *gp <= '9') {
            w = w * 10 + (*gp - '0');
            gp++;
        }

        fwd_head[u + 1]++;
        rev_head[v + 1]++;
        edge_list.push_back({u, v, w});
    }

    for (uint32_t i = 0; i <= n; ++i) {
        fwd_head[i + 1] += fwd_head[i];
        rev_head[i + 1] += rev_head[i];
    }

    std::vector<uint32_t> fwd_to(m);
    std::vector<uint64_t> fwd_w(m);
    std::vector<uint32_t> cur_fwd = fwd_head;

    std::vector<uint32_t> rev_to(m);
    std::vector<uint64_t> rev_w(m);
    std::vector<uint32_t> cur_rev = rev_head;

    for (const auto& e : edge_list) {
        uint32_t fp = cur_fwd[e.u]++;
        fwd_to[fp] = e.v;
        fwd_w[fp] = e.w;

        uint32_t rp = cur_rev[e.v]++;
        rev_to[rp] = e.u;
        rev_w[rp] = e.w;
    }
    std::vector<Edge>().swap(edge_list);

    // Parse queries
    ferocious::MappedFile qmf(queries_path.c_str());
    if (!qmf.ok) return 1;

    const char* qp = static_cast<const char*>(qmf.data);
    const char* qend = qp + qmf.size;

    std::vector<std::pair<uint32_t, uint32_t>> queries;
    while (qp < qend) {
        while (qp < qend && (*qp == ' ' || *qp == '\t' || *qp == '\r' || *gp == '\n' || *qp == '\n')) qp++;
        if (qp >= qend) break;

        uint32_t s = 0;
        while (qp < qend && *qp >= '0' && *qp <= '9') {
            s = s * 10 + (*qp - '0');
            qp++;
        }
        while (qp < qend && (*qp == ' ' || *qp == '\t')) qp++;

        uint32_t t = 0;
        while (qp < qend && *qp >= '0' && *qp <= '9') {
            t = t * 10 + (*qp - '0');
            qp++;
        }
        queries.push_back({s, t});
    }

    size_t num_queries = queries.size();
    std::vector<int64_t> answers(num_queries, -1);
    std::atomic<size_t> query_idx{0};

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    std::vector<std::thread> workers;

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&]() {
            std::vector<uint64_t> f_dist(n, INF64);
            std::vector<uint64_t> r_dist(n, INF64);
            std::vector<uint32_t> f_visited;
            std::vector<uint32_t> r_visited;
            f_visited.reserve(2048);
            r_visited.reserve(2048);

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
                            uint32_t v = fwd_to[e];
                            uint64_t nd = d + fwd_w[e];
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
                            uint32_t v = rev_to[e];
                            uint64_t nd = d + rev_w[e];
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

    for (auto& w : workers) {
        w.join();
    }

    // Write distances.txt
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
