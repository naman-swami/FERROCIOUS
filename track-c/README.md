# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

FERROCIOUS processes hostile CSV records and joins them against the regions lookup table:
- Memory-mapped streaming parser capable of handling mixed CRLF/LF line endings, quoted fields with embedded commas and newlines, escaped quotes (`""`), and mid-file UTF-8 BOM markers (`\xEF\xBB\xBF`).
- Flat hash table mapping `region_id` to 3-letter country codes with fallback to `_UNMATCHED`.
- 100% exact integer arithmetic for currency: amounts are parsed directly into integer cents, avoiding all binary floating-point representation and rounding divergences.
- Per-country aggregation of counts, 64-bit ID sums, distinct name counts (via hash set), and byte lengths of unescaped names and notes.

## Dependencies

None ($D = 1.00$). Standard C++20 library only.

## With another 24 hours

- Branchless SIMD quote-masking using AVX2 carryless multiplication (PCLMULQDQ) for full vector-parallel CSV parsing.
- Robin Hood open-addressing hash table for distinct name deduplication to eliminate dynamic heap allocations.
