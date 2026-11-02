# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

FERROCIOUS reads the 3.1 million log lines once, on up to 12 threads, and writes `summary.txt` with high-performance C++20 standard library and AVX2 intrinsics:
- **Parse:** Input is read via parallel L2-resident `pread` chunks (128 KB) from an atomic counter. Newlines are found with 64-byte AVX2 bitmasks. Each line is read from both ends (endpoint from the front, status and latency from the back), so lines are independent of each other. The hour is read from fixed byte offsets of the timestamp; there is zero date parsing.
- **Endpoints:** One shared thread-safe table maps an endpoint's bytes to a global ID with hardware vector compares and an endpoint arena (`g_ep_arena`). Lookups take no lock; only the first sighting of an endpoint does.
- **Records:** Every line becomes a compact record in a per-thread, per-hour block list from a single bump allocator.
- **Aggregate:** Hours are processed in parallel across threads. Each is grouped by endpoint in byte order. Small groups ($n \le 64$) are sorted outright; larger ones use linear $O(N)$ `std::nth_element` for the nearest-rank p50, p95 and p99 (`(p * n + 99) / 100`). Error rate is `errors * 1,000,000 / count` in integer arithmetic.
- **Output:** Each hour is formatted into its own buffer with integer-only printing, then written sequentially to `summary.txt`.

## Dependencies

None ($D = 1.00$). Standard C++20 library and AVX2 intrinsics only.

## With another 24 hours

- Replace `nth_element` on the few hundred large groups with a log-linear histogram select.
