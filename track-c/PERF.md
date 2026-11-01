# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.44 | 367 | 0 |
| 1 | Baseline C++20 engine: mmap CSV parsing, dense region-to-country lookup array | run-76a47e31b94a | 0.569 | 30 | 40.29 |
| 2 | Parallel CSV record chunking with CRLF boundaries & 131,072-bit country bitsets | run-b9837931ea47 | 0.171 | 75 | 48.90 |

- **Step 1:** Replaced pandas CSV parsing with zero-dependency C++20 engine using raw memory mapping, strict fixed-point cents parsing, and a dense integer direct-lookup array for region-to-country mapping. Runtime dropped from 14.44s to 0.569s (`run-76a47e31b94a`).
- **Step 2:** Leveraged CSV RFC-4180 guarantees to partition raw 200 MB CSV data across 12 hardware threads at exact `\r\n` record boundaries. Replaced dynamic hash sets for distinct customer names with per-country 131,072-bit bitsets and 8 MB static arena string pooling, merging thread bitsets with bitwise OR and SIMD `std::popcount`. Runtime dropped from 0.569s to 0.171s (`run-b9837931ea47`).
