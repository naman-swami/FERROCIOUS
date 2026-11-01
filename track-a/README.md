# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

FERROCIOUS parses 3.1 million HTTP log lines with a high-performance multithreaded C++20 engine:
- Memory-mapped I/O (`mmap` / `MapViewOfFile`) with sequential readahead (`MADV_WILLNEED`).
- Parallel chunk parsing across 12 hardware threads with newline boundary alignment.
- Zero-copy string view slicing and branchless integer parsing for HTTP status codes and response latencies.
- Integer-arithmetic percentile aggregation using nearest-rank formula `(p * n + 99) / 100` and parts-per-million error rate `(errors * 1,000,000) / count`.
- Fast buffered output stream (`OutBuf`) with custom integer formatting.

## Dependencies

None ($D = 1.00$). Standard C++20 library only.

## With another 24 hours

- Implement AVX2 32-byte SIMD newline masking and vectorized token scanning.
- Replace thread-local maps with a dense, direct-indexed array using perfect hashing for the 1,842 endpoint templates.
