# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

FERROCIOUS evaluates 50,000 boolean and exact phrase queries over the text corpus:
- Two-phase query-driven pipeline: reads `queries.txt` first to extract the active query vocabulary, filtering out unneeded terms.
- Single-pass memory-mapped tokenization with ASCII fast lowercasing and maximal runs of `[a-z0-9]`.
- Compact inverted index storing sorted document ID postings and positional delta offsets.
- Fast left-associative boolean evaluation for `AND`, `OR`, and binary set difference (`NOT`).
- Consecutive positional matching for `PHRASE` queries.
- Top-10 smallest document ID selection with 64-bit integer sum accumulators.

## Dependencies

None ($D = 1.00$). Standard C++20 library only.

## With another 24 hours

- Implement Roaring Bitmap / AVX2 bitset intersections for dense boolean posting lists.
- Galloping search (exponential search) for unbalanced posting list intersections.
