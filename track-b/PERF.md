# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 56.35 | 2,200 | 0 |
| 1 | Baseline C++20 engine: mmap IO, parallel docs indexing with std::unordered_map postings | run-097be6b7c1d5 | 2.458 | 343 | 35.55 |
| 2 | Dense integer vocabulary mapping & ping-pong query evaluation: eliminated per-query allocations | run-8036c4156bbe | 1.349 | 250 | 43.09 |
| 3 | Flat 262k open-addressing register-hash vocabulary table: zero-allocation token lookup | run-cbfc06a | 1.085 | 250 | 45.40 |
| 4 | AVX2 SIMD parallel chunk doc scan, 12-thread parallel index merge & zero-allocation phrase verification | run-72dbc688263e | 1.032 | 248 | 45.66 |

- **Step 1:** Implemented zero-dependency C++20 parallel document tokenizer and inverted index builder, eliminating Python regex and heap thrashing. Runtime dropped from 56.35s to 2.458s (`run-097be6b7c1d5`).
- **Step 2:** Extracted unique active query tokens into contiguous 32-bit integer IDs to replace string hash table lookups during indexing with direct vector array accesses. Implemented alternating ping-pong scratch buffers (`buf_a` / `buf_b`) for boolean set intersections and phrase candidate filtering, eliminating hundreds of thousands of heap allocations per query run. Runtime dropped from 2.458s to 1.349s with memory reduced to 250 MB (`run-8036c4156bbe`).
- **Step 3:** Replaced dynamic string token hash table with a flat 262k open-addressing register-hash table evaluated directly in CPU registers via 64-bit FNV-1a. Dropped runtime from 1.349s to 1.085s (`run-cbfc06a`).
- **Step 4:** Replaced single-threaded document boundary pre-scan with AVX2 SIMD parallel chunk scan across 12 threads (cutting doc scan from 741 ms to 50 ms), parallelized global posting list merge across 12 threads (cutting merge from 289 ms to 60 ms), and eliminated heap allocations during PHRASE candidate verification.
