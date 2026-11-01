# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 72.69 | 893 | 0 |
| 1 | Baseline C++20 engine: mmap parsing, forward CSR graph, 4-ary Min-Heap Dijkstra | run-c7f5cca3b836 | 1.783 | 327 | 36.41 |
| 2 | Parallel 12-thread edge parsing and atomic scatter CSR construction with bidirectional Dijkstra | run-916e3679e7fc | 1.247 | 466 | 38.12 |

- **Step 1:** Implemented zero-dependency C++20 CSR representation and a custom cache-conscious 4-ary Min-Heap for single-source shortest paths, eliminating Python networkx overhead. Runtime dropped from 72.69s to 1.783s (`run-c7f5cca3b836`).
- **Step 2:** Accelerated graph construction by chunking the 92 MB graph file across 12 threads with parallel integer parsing and atomic scatter into forward and reverse CSR arrays, followed by 12-thread bidirectional Dijkstra with early-termination pruning ($d_f + d_r \ge \mu$). Runtime dropped from 1.783s to 1.247s (`run-916e3679e7fc`).
