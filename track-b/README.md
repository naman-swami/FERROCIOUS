# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

FERROCIOUS answers 50,000 boolean and exact phrase queries over the corpus:
- It reads `queries.txt` first and indexes only the terms the queries use.
- Two passes over the memory-mapped corpus, split at line boundaries across threads. Pass 1 counts the document frequency of every query term. Pass 2 writes the sorted doc ids of every term into one flat CSR array.
- Tokenization is byte-based ([a-z0-9] runs, ASCII lowercase). Token boundaries come from AVX2 bitmasks, and each token is looked up in a flat open-addressing table. Lookups are prefetched 16 tokens ahead.
- Terms in many documents also get a bitset, so AND and NOT against them cost one probe per surviving candidate.
- BOOL queries run strictly left to right on sorted lists. Short lists gallop into long ones; the others merge. The first operand is read in place.
- PHRASE queries intersect the term lists rarest-first, then re-scan each surviving candidate line and match the phrase with KMP.
- Output is the count, the exact 64-bit sum of ids and the ten smallest ids.
- No positional index is stored: phrases are verified on the text.

## Dependencies

None ($D = 1.00$). Standard C++20 library and AVX2 intrinsics only.

## With another 24 hours

- Segment-copy unions and differences when one list is much shorter than the other.
- Store byte offsets for rare terms so phrases can be verified without re-scanning the whole line.
- Roaring-style containers instead of one bitset size for all frequent terms.
