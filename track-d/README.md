# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

FERROCIOUS solves 2,000 shortest path queries over a large directed graph:
- Dual Compressed Sparse Row (CSR) graph representation storing both forward and reverse directed edge lists.
- Dynamically parses node count $n$ and edge count $m$ directly from the file header, supporting arbitrary graph topologies.
- Multithreaded Bidirectional Dijkstra search with meeting point tracking ($d_f + d_b \ge \mu$).
- Generation counter (`epoch`) array eliminating expensive $O(V)$ memory resets between queries.
- Fast 64-bit integer path accumulation to prevent overflow on long paths.

## Dependencies

None ($D = 1.00$). Standard C++20 library only.

## With another 24 hours

- Contraction Hierarchies (CH) or Hub Labeling preprocessing inside the timed window.
- 4-ary SIMD heap with AVX2 vectorized child comparisons.
