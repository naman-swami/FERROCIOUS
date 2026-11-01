# ⚡ FERROCIOUS

> **Speed Demon 2026: Grand Prize Championship Entry**  
> *Iron-Fast, Iron-Clad Systems Performance Engineering across Tracks A, B, C, and D.*

---

## 🏛️ System Architecture

FERROCIOUS is engineered from the ground up for maximum computational density, predictable cache utilization, and strict byte-for-byte output correctness on the Falkenstein scoring box (Intel Core i5-13500 Raptor Lake):

* **Zero External Dependencies ($D = 1.00$):** 100% Standard C++20. No external crates, third-party libraries, or runtime bloat. Maximizes the scoring dependency factor $\left(\frac{D_{\text{ref}}}{D}\right)^{0.15} = 1.00$.
* **Four-Track Hedge Strategy:** Submits complete, high-performance engines across all four tracks ([Track A](track-a/README.md), [Track B](track-b/README.md), [Track C](track-c/README.md), [Track D](track-d/README.md)). Under the Speed Demon scoring rules, the overall championship score is the geometric mean of the top 3 tracks, automatically discarding any weaker 4th run while opening full eligibility for single-track awards.
* **Track A ([Ingest](track-a/README.md)):** High-throughput multithreaded log parser utilizing `mmap` with `MADV_SEQUENTIAL`, chunk boundary alignment across 12 hardware threads, branchless status and latency integer parsers, and nearest-rank percentile computation.
* **Track B ([Index](track-b/README.md)):** Query-driven inverted index pre-harvesting vocabulary from `queries.txt` to eliminate >95% of corpus token indexing, delta-varint compressed postings, SIMD tokenization, and parallel evaluation of left-associative boolean (`AND`, `OR`, `NOT`) and exact phrase queries.
* **Track C ([Join](track-c/README.md)):** Streaming RFC 4180 CSV parser resilient to quoted fields, embedded newlines, escaped quotes, and mid-file UTF-8 BOMs (`0xEF 0xBB 0xBF`). 100% exact integer-cents arithmetic with zero floating-point representation or rounding errors.
* **Track D ([Traverse](track-d/README.md)):** Dual Compressed Sparse Row (CSR) graph layout with generation counter pattern ($O(1)$ state resets between queries), 64-byte cache-line aligned 4-ary Min-Heap, and multithreaded Bidirectional Dijkstra shortest-path searches.

---

## 📂 Repository Structure

Each track directory is self-contained with its own allowlist-pinned Dockerfile, specification manifest, documentation, and source code:

| Directory | Track | Baseline $T_{\text{ref}}$ | Baseline $M_{\text{ref}}$ | Performance Log |
|---|---|---|---|---|
| [`track-a/`](track-a/) | Ingest | 32.05 s | 228 MB | [`track-a/PERF.md`](track-a/PERF.md) |
| [`track-b/`](track-b/) | Index | 56.35 s | 2,200 MB | [`track-b/PERF.md`](track-b/PERF.md) |
| [`track-c/`](track-c/) | Join | 14.44 s | 367 MB | [`track-c/PERF.md`](track-c/PERF.md) |
| [`track-d/`](track-d/) | Traverse | 72.69 s | 893 MB | [`track-d/PERF.md`](track-d/PERF.md) |

---

## 🚀 Building & Local Testing

The repository uses the official `check.py` runner to verify offline image builds and sandbox execution:

```bash
# Check Track A
python3 check.py a --data /path/to/data/a

# Check Track B
python3 check.py b --data /path/to/data/b

# Check Track C
python3 check.py c --data /path/to/data/c

# Check Track D
python3 check.py d --data /path/to/data/d
```

### Native Compilation

To compile locally using GCC (C++20):

```bash
g++ -O3 -std=c++20 -march=native -pthread -Itrack-a track-a/main.cpp -o track-a/solution
g++ -O3 -std=c++20 -march=native -pthread -Itrack-b track-b/main.cpp -o track-b/solution
g++ -O3 -std=c++20 -march=native -pthread -Itrack-c track-c/main.cpp -o track-c/solution
g++ -O3 -std=c++20 -march=native -pthread -Itrack-d track-d/main.cpp -o track-d/solution
```

---

## 📄 License

Licensed under the [MIT License](LICENSE).
