// FERROCIOUS Track D — Official Shortest Paths Engine (specs/d-traverse.md)
// Zero External Dependencies (D = 1.00). C++20.

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include "ferocious_common.hpp"

namespace {

constexpr uint64_t INF64 = UINT64_MAX;

struct Edge {
    uint32_t to;
    uint64_t weight;
};

// 4-ary Min-Heap for Dijkstra
struct MinHeap4 {
    struct Node {
        uint64_t dist;
        uint32_t u;
    };
    std::vector<Node> tree;

    void clear() { tree.clear(); }
    bool empty() const { return tree.empty(); }

    void push(uint64_t d, uint32_t u) {
        size_t i = tree.size();
        tree.push_back({d, u});
        while (i > 0) {
            size_t p = (i - 1) >> 2; // parent in 4-ary heap
            if (tree[p].dist <= tree[i].dist) break;
            std::swap(tree[p], tree[i]);
            i = p;
        }
    }

    Node pop() {
        Node min_node = tree[0];
        tree[0] = tree.back();
        tree.pop_back();
        if (!tree.empty()) {
            size_t i = 0;
            size_t n = tree.size();
            while (true) {
                size_t best = i;
                size_t child = (i << 2) + 1;
                for (size_t c = 0; c < 4 && child + c < n; ++c) {
                    if (tree[child + c].dist < tree[best].dist) {
                        best = child + c;
                    }
                }
                if (best == i) break;
                std::swap(tree[i], tree[best]);
                i = best;
            }
        }
        return min_node;
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
    if (!gmf.ok) {
        std::cerr << "Cannot open " << graph_path << "\n";
        return 1;
    }

    ferocious::MappedFile qmf(queries_path.c_str());
    if (!qmf.ok) {
        std::cerr << "Cannot open " << queries_path << "\n";
        return 1;
    }

    const char* gp = static_cast<const char*>(gmf.data);
    const char* gend = gp + gmf.size;

    // Parse header: n m
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

    // Count degrees to build CSR
    std::vector<uint32_t> head(n + 2, 0);
    struct RawEdge {
        uint32_t u, v;
        uint64_t w;
    };
    std::vector<RawEdge> edges;
    edges.reserve(m);

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

        head[u + 1]++;
        edges.push_back({u, v, w});
    }

    // Prefix sums for CSR
    for (uint32_t i = 0; i <= n; ++i) {
        head[i + 1] += head[i];
    }

    std::vector<uint32_t> to(m);
    std::vector<uint64_t> weight(m);
    std::vector<uint32_t> cur_pos = head;

    for (const auto& e : edges) {
        uint32_t p = cur_pos[e.u]++;
        to[p] = e.v;
        weight[p] = e.w;
    }

    // Parse queries
    const char* qp = static_cast<const char*>(qmf.data);
    const char* qend = qp + qmf.size;

    std::vector<std::pair<uint32_t, uint32_t>> queries;
    while (qp < qend) {
        while (qp < qend && (*qp == ' ' || *qp == '\t' || *qp == '\r' || *qp == '\n')) qp++;
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

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 6;
    if (num_threads > 12) num_threads = 12;

    std::atomic<size_t> query_idx(0);
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&]() {
            std::vector<uint64_t> dist(n, INF64);
            std::vector<uint32_t> visited_nodes;
            visited_nodes.reserve(1024);
            MinHeap4 heap;

            while (true) {
                size_t qid = query_idx.fetch_add(1, std::memory_order_relaxed);
                if (qid >= num_queries) break;

                uint32_t s = queries[qid].first;
                uint32_t t = queries[qid].second;

                if (s == t) {
                    answers[qid] = 0;
                    continue;
                }

                // Reset distances for touched nodes
                for (uint32_t u : visited_nodes) {
                    dist[u] = INF64;
                }
                visited_nodes.clear();
                heap.clear();

                dist[s] = 0;
                visited_nodes.push_back(s);
                heap.push(0, s);

                int64_t ans = -1;

                while (!heap.empty()) {
                    auto top = heap.pop();
                    uint64_t d = top.dist;
                    uint32_t u = top.u;

                    if (d > dist[u]) continue;
                    if (u == t) {
                        ans = static_cast<int64_t>(d);
                        break;
                    }

                    uint32_t start_edge = head[u];
                    uint32_t end_edge   = head[u + 1];

                    for (uint32_t e = start_edge; e < end_edge; ++e) {
                        uint32_t v = to[e];
                        uint64_t nd = d + weight[e];

                        if (nd < dist[v]) {
                            if (dist[v] == INF64) {
                                visited_nodes.push_back(v);
                            }
                            dist[v] = nd;
                            heap.push(nd, v);
                        }
                    }
                }

                answers[qid] = ans;
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    // 4. Write distances.txt
    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    FILE* fp = fopen(distances_path.c_str(), "wb");
    if (!fp) {
        std::cerr << "Cannot open " << distances_path << "\n";
        return 1;
    }
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
