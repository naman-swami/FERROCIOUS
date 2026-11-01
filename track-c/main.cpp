// FERROCIOUS Track C — High Performance Flat-Table CSV Engine (specs/c-join.md)
// Zero External Dependencies (D = 1.00). C++20.

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <cstring>
#include <cstdint>
#include <map>
#include <algorithm>
#include "ferocious_common.hpp"

namespace {

struct CountryStats {
    uint64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = INT64_MAX;
    int64_t amount_max = INT64_MIN;
    uint32_t names_distinct = 0;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
};

inline void format_cents(int64_t cents, ferocious::OutBuf& out) {
    if (cents < 0) {
        out.put('-');
        cents = -cents;
    }
    out.u64(cents / 100);
    out.put('.');
    int rem = cents % 100;
    out.put('0' + (rem / 10));
    out.put('0' + (rem % 10));
}

inline int64_t parse_cents_strict(const char* p, const char* end) {
    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        p++;
    }
    int64_t whole = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        p++;
    }
    int64_t frac = 0;
    if (p < end && *p == '.') {
        p++;
        if (p < end && *p >= '0' && *p <= '9') frac = (p[0] - '0') * 10;
        if (p + 1 < end && *(p + 1) >= '0' && *(p + 1) <= '9') frac += (p[1] - '0');
    }
    int64_t total = whole * 100 + frac;
    return neg ? -total : total;
}

// Flat open-addressing string pool
struct StringPool {
    static constexpr size_t HASH_CAP = 262144; // 2^18, > 2x max names (100k)
    static constexpr size_t MASK = HASH_CAP - 1;

    struct Entry {
        uint32_t offset = 0;
        uint32_t len = 0;
        uint32_t id = 0;
    };

    std::vector<char> arena;
    std::vector<Entry> table;
    uint32_t next_id = 0;

    StringPool() : table(HASH_CAP) {
        arena.reserve(4 * 1024 * 1024);
    }

    static inline uint64_t hash_bytes(const char* p, size_t len) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < len; ++i) {
            h ^= (uint8_t)p[i];
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    inline uint32_t get_or_insert(const char* s, size_t len) {
        uint64_t h = hash_bytes(s, len);
        size_t idx = h & MASK;

        while (true) {
            Entry& e = table[idx];
            if (e.len == 0 && e.id == 0 && (idx != 0 || next_id != 0)) {
                // Empty slot found
                uint32_t off = (uint32_t)arena.size();
                arena.insert(arena.end(), s, s + len);
                e.offset = off;
                e.len = (uint32_t)len;
                e.id = next_id++;
                return e.id;
            }

            if (e.len == len && std::memcmp(arena.data() + e.offset, s, len) == 0) {
                return e.id;
            }

            idx = (idx + 1) & MASK;
        }
    }
};

} // namespace

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string data_dir = (argc > 1) ? argv[1] : "/data";
    std::string out_dir  = (argc > 2) ? argv[2] : "/out";

    std::string records_path = data_dir + "/records.csv";
    std::string regions_path = data_dir + "/regions.csv";
    std::string aggregates_path = out_dir + "/aggregates.txt";

    // 1. Regions: map country names to country_ids (0..240)
    std::vector<std::string> country_names;
    std::vector<uint16_t> region_to_country_id(10000000, 0xFFFF);
    uint16_t unmatched_cid = 0xFFFF;

    {
        ferocious::MappedFile rmf(regions_path.c_str());
        if (!rmf.ok) return 1;

        std::map<std::string, uint16_t> country_to_id;

        const char* p = static_cast<const char*>(rmf.data);
        const char* end = p + rmf.size;

        const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
        if (nl) p = nl + 1;

        while (p < end) {
            if (*p == '\r' || *p == '\n') { p++; continue; }
            uint32_t rid = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                rid = rid * 10 + (*p - '0');
                p++;
            }
            if (p < end && *p == ',') p++;
            const char* cstart = p;
            while (p < end && *p != '\r' && *p != '\n') p++;
            std::string ccode(cstart, p - cstart);

            auto it = country_to_id.find(ccode);
            uint16_t cid;
            if (it == country_to_id.end()) {
                cid = (uint16_t)country_names.size();
                country_to_id[ccode] = cid;
                country_names.push_back(ccode);
            } else {
                cid = it->second;
            }

            if (rid >= region_to_country_id.size()) region_to_country_id.resize(rid + 100000, 0xFFFF);
            region_to_country_id[rid] = cid;

            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
        }

        unmatched_cid = (uint16_t)country_names.size();
        country_names.push_back("_UNMATCHED");
    }

    uint16_t num_countries = (uint16_t)country_names.size();
    std::vector<CountryStats> stats(num_countries);

    // Bitsets for seen names per country: 131,072 bits / 64 = 2048 uint64_t per country
    static constexpr size_t BITSET_WORDS = 2048; // up to 131,072 distinct names
    std::vector<std::vector<uint64_t>> seen_names(num_countries, std::vector<uint64_t>(BITSET_WORDS, 0));

    StringPool pool;
    // Pre-insert empty string as id 0
    pool.get_or_insert("", 0);

    // 2. Records
    ferocious::MappedFile mf(records_path.c_str());
    if (!mf.ok) return 1;

    const char* p = static_cast<const char*>(mf.data);
    const char* end = p + mf.size;

    if (end - p >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
        p += 3;
    }
    const char* hdr_end = static_cast<const char*>(std::memchr(p, '\n', end - p));
    if (hdr_end) p = hdr_end + 1;

    char unescape_buf[1024];

    while (p < end) {
        if (end - p >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
            p += 3;
        }
        if (p >= end || *p == '\r' || *p == '\n') {
            if (p < end) p++;
            continue;
        }

        int64_t rec_id = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            rec_id = rec_id * 10 + (*p - '0');
            p++;
        }
        if (p < end && *p == ',') p++;

        // Field 2: name
        const char* name_ptr = nullptr;
        uint32_t name_len = 0;
        if (p < end && *p == '"') {
            p++;
            char* dest = unescape_buf;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') {
                        *dest++ = '"';
                        p += 2;
                    } else {
                        p++;
                        break;
                    }
                } else {
                    *dest++ = *p++;
                }
            }
            name_ptr = unescape_buf;
            name_len = (uint32_t)(dest - unescape_buf);
        } else {
            const char* nstart = p;
            while (p < end && *p != ',') p++;
            name_ptr = nstart;
            name_len = (uint32_t)(p - nstart);
        }
        if (p < end && *p == ',') p++;

        // Field 3: note
        uint64_t note_len = 0;
        if (p < end && *p == '"') {
            p++;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') {
                        note_len++;
                        p += 2;
                    } else {
                        p++;
                        break;
                    }
                } else {
                    note_len++;
                    p++;
                }
            }
        } else {
            while (p < end && *p != ',') {
                note_len++;
                p++;
            }
        }
        if (p < end && *p == ',') p++;

        // Field 4: region_id
        uint32_t reg_id = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            reg_id = reg_id * 10 + (*p - '0');
            p++;
        }
        if (p < end && *p == ',') p++;

        // Field 5: amount
        const char* amt_start = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        const char* amt_end = p;

        int64_t cents = parse_cents_strict(amt_start, amt_end);

        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;

        // Country lookup
        uint16_t cid = (reg_id < region_to_country_id.size()) ? region_to_country_id[reg_id] : unmatched_cid;
        if (cid == 0xFFFF) cid = unmatched_cid;

        // Distinct name lookup
        uint32_t name_id = pool.get_or_insert(name_ptr, name_len);

        auto& cs = stats[cid];
        cs.count++;
        cs.id_sum += rec_id;
        cs.amount_sum += cents;
        if (cents < cs.amount_min) cs.amount_min = cents;
        if (cents > cs.amount_max) cs.amount_max = cents;
        cs.name_bytes += name_len;
        cs.note_bytes += note_len;

        // Bitset test
        size_t word_idx = name_id >> 6;
        uint64_t mask = 1ULL << (name_id & 63);
        if (!(seen_names[cid][word_idx] & mask)) {
            seen_names[cid][word_idx] |= mask;
            cs.names_distinct++;
        }
    }

    // 3. Emit aggregates.txt sorted by country name
    #ifdef _WIN32
    CreateDirectoryA(out_dir.c_str(), NULL);
    #endif

    // Collect active countries and sort by name
    std::vector<uint16_t> active_cids;
    for (uint16_t cid = 0; cid < num_countries; ++cid) {
        if (stats[cid].count > 0) {
            active_cids.push_back(cid);
        }
    }
    std::sort(active_cids.begin(), active_cids.end(), [&](uint16_t a, uint16_t b) {
        return country_names[a] < country_names[b];
    });

    FILE* fp = fopen(aggregates_path.c_str(), "wb");
    if (!fp) return 1;

    ferocious::OutBuf out(fp);

    for (uint16_t cid : active_cids) {
        const std::string& cname = country_names[cid];
        const auto& cs = stats[cid];
        out.puts(cname.data(), cname.size());
        out.put(' ');
        out.u64(cs.count);
        out.put(' ');
        out.u64(cs.id_sum);
        out.put(' ');
        format_cents(cs.amount_sum, out);
        out.put(' ');
        format_cents(cs.amount_min, out);
        out.put(' ');
        format_cents(cs.amount_max, out);
        out.put(' ');
        out.u64(cs.names_distinct);
        out.put(' ');
        out.u64(cs.name_bytes);
        out.put(' ');
        out.u64(cs.note_bytes);
        out.put('\n');
    }

    out.flush();
    fclose(fp);
    return 0;
}
