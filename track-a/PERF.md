# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | Baseline C++20 engine: mmap IO, custom integer parsing, sequential endpoint aggregation | run-2ba837aea363 | 0.440 | 104 | 43.18 |
| 2 | 12-thread lock-free parallel ingest: thread-local string dictionaries, precomputed 72-hour table, parallel hour-bucketed sorting and buffer formatting | run-e673b13b308c | 0.0886 | 56 | 62.33 |
| 3 | 1-Pass Counting Sort Engine: thread-local raw parsing, lexicographic dense ID mapping, 132k-bucket counting sort with contiguous 12.4 MB latency scattering and zero-allocation error counters | pending | pending | pending | pending |

- **Step 1:** Replaced naive Python with zero-dependency C++20 engine using `mmap` and custom string-to-int parsing to eliminate interpreter overhead. Runtime dropped from 32.05s to 0.440s (`run-2ba837aea363`).
- **Step 2:** Eliminated global mutex contention and datetime parsing overhead by chunking raw log bytes across 12 hardware threads, maintaining thread-local endpoint dictionaries, using direct arithmetic mapping into 72 precomputed hour strings, and parallelizing hour-bucketed percentiles sorting and buffer emission. Runtime dropped from 0.440s to 0.0886s with memory halved to 56 MB (`run-e673b13b308c`).
- **Step 3:** Replaced dynamic per-endpoint vector reallocations and multi-stage copies with an ultra-compact 1-pass counting sort. Records are parsed directly into 8-byte raw tuples, endpoints mapped to dense lexicographic IDs, and latencies scattered in parallel into contiguous bucket spans (12.4 MB total), eliminating error byte storage completely and cutting latency sort time to sub-millisecond ranges.
