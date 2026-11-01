# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | Baseline C++20 engine: mmap IO, custom integer parsing, sequential endpoint aggregation | run-2ba837aea363 | 0.440 | 104 | 43.18 |
| 2 | 12-thread lock-free parallel ingest: thread-local string dictionaries, precomputed 72-hour table, parallel hour-bucketed sorting and buffer formatting | run-e673b13b308c | 0.0886 | 56 | 62.33 |
| 3 | Direct integer formatters: replaced snprintf formatting with branchless fast_append_u32/u64 and OutBuf streaming | pending | | | |

- **Step 1:** Replaced naive Python with zero-dependency C++20 engine using `mmap` and custom string-to-int parsing to eliminate interpreter overhead. Runtime dropped from 32.05s to 0.440s (`run-2ba837aea363`).
- **Step 2:** Eliminated global mutex contention and datetime parsing overhead by chunking raw log bytes across 12 hardware threads, maintaining thread-local endpoint dictionaries, using direct arithmetic mapping into 72 precomputed hour strings, and parallelizing hour-bucketed percentiles sorting and buffer emission. Runtime dropped from 0.440s to 0.0886s with memory halved to 56 MB (`run-e673b13b308c`).
- **Step 3:** Replaced slow libc `snprintf` serialization in the summary emission loop with branchless `fast_append_u32` and `fast_append_u64` direct char buffer streaming, eliminating formatting overhead across all 72 hours and endpoint buckets.
