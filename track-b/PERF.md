# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 56.35 | 2,200 | 0 |
| 1 | Baseline C++20 engine: mmap IO, parallel docs indexing with std::unordered_map postings | run-097be6b7c1d5 | 2.458 | 343 | 35.55 |
| 2 | Dense integer vocabulary mapping & ping-pong query evaluation: eliminated per-query allocations | run-8036c4156bbe | 1.349 | 250 | 43.09 |

- **Step 1:** Implemented zero-dependency C++20 parallel document tokenizer and inverted index builder, eliminating Python regex and heap thrashing. Runtime dropped from 56.35s to 2.458s (`run-097be6b7c1d5`).
- **Step 2:** Extracted unique active query tokens into contiguous 32-bit integer IDs to replace string hash table lookups during indexing with direct vector array accesses. Implemented alternating ping-pong scratch buffers (`buf_a` / `buf_b`) for boolean set intersections and phrase candidate filtering, eliminating hundreds of thousands of heap allocations per query run. Runtime dropped from 2.458s to 1.349s with memory reduced to 250 MB (`run-8036c4156bbe`).
