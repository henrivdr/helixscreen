// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_types.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

// ============================================================================
// Fuzzy String Matching
// ============================================================================

namespace {

/// Levenshtein edit distance between two strings.
/// Uses a single-row DP approach (O(min(a,b)) space).
size_t levenshtein(const std::string& a, const std::string& b) {
    const size_t m = a.size();
    const size_t n = b.size();
    if (m == 0)
        return n;
    if (n == 0)
        return m;

    std::vector<size_t> prev(n + 1);
    for (size_t j = 0; j <= n; ++j)
        prev[j] = j;

    for (size_t i = 1; i <= m; ++i) {
        size_t prev_diag = prev[0];
        prev[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            size_t tmp = prev[j];
            if (a[i - 1] == b[j - 1]) {
                prev[j] = prev_diag;
            } else {
                prev[j] = 1 + std::min({prev_diag, prev[j], prev[j - 1]});
            }
            prev_diag = tmp;
        }
    }
    return prev[n];
}

/// Maximum edit distance allowed for a given search term length.
/// <=4 chars: 1 edit (tight for short words). >4 chars: 2 edits.
size_t fuzzy_threshold(size_t term_length) {
    return (term_length <= 4) ? 1 : 2;
}

/// Check if a term looks like a numeric ID (digits, or # followed by digits).
/// Fuzzy matching is skipped for these since typos in numbers are semantically different.
bool is_numeric_term(const std::string& term) {
    if (term.empty())
        return false;
    size_t start = (term[0] == '#') ? 1 : 0;
    if (start >= term.size())
        return false;
    return std::all_of(term.begin() + static_cast<ptrdiff_t>(start), term.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

/// Check if a search term fuzzy-matches any word in the searchable text.
/// Words are split on spaces. Returns true if any word is within edit distance threshold.
bool fuzzy_match_any_word(const std::string& term, const std::string& searchable) {
    // Don't fuzzy-match numeric terms (IDs) — exact substring is sufficient
    if (is_numeric_term(term))
        return false;

    size_t threshold = fuzzy_threshold(term.size());

    std::istringstream words(searchable);
    std::string word;
    while (words >> word) {
        // Skip words with very different lengths — they can't be typo matches
        auto len_diff =
            (term.size() > word.size()) ? term.size() - word.size() : word.size() - term.size();
        if (len_diff > threshold)
            continue;

        if (levenshtein(term, word) <= threshold) {
            return true;
        }
    }
    return false;
}

} // namespace

// ============================================================================
// Spool Filtering
// ============================================================================

std::vector<SpoolInfo> filter_spools(const std::vector<SpoolInfo>& spools,
                                     const std::string& query) {
    // Empty or whitespace-only query returns all spools.
    // The stream >> term loop skips whitespace, so terms will be empty for whitespace-only input.
    if (query.empty()) {
        return spools;
    }

    // Split query into lowercase terms (space-separated)
    std::vector<std::string> terms;
    std::istringstream stream(query);
    std::string term;
    while (stream >> term) {
        std::transform(term.begin(), term.end(), term.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        terms.push_back(std::move(term));
    }

    if (terms.empty()) {
        return spools;
    }

    std::vector<SpoolInfo> result;
    result.reserve(spools.size());

    for (const auto& spool : spools) {
        // Build searchable text: "#ID vendor material color_name location"
        std::string searchable = "#" + std::to_string(spool.id) + " " + spool.vendor + " " +
                                 spool.material + " " + spool.color_name + " " + spool.location;

        // Lowercase the searchable text
        std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // All terms must match (AND logic): exact substring first, fuzzy fallback
        bool all_match =
            std::all_of(terms.begin(), terms.end(), [&searchable](const std::string& t) {
                // Fast path: exact substring match
                if (searchable.find(t) != std::string::npos) {
                    return true;
                }
                // Slow path: fuzzy match against individual words
                return fuzzy_match_any_word(t, searchable);
            });

        if (all_match) {
            result.push_back(spool);
        }
    }

    return result;
}

void sort_spools_by_recency(std::vector<SpoolInfo>& spools) {
    std::sort(spools.begin(), spools.end(), [](const SpoolInfo& a, const SpoolInfo& b) {
        const bool a_null = a.last_used.empty();
        const bool b_null = b.last_used.empty();
        if (a_null != b_null)
            return !a_null; // used spools before never-used
        if (!a_null && a.last_used != b.last_used)
            return a.last_used > b.last_used; // newer use first
        // Never-used spools: order by creation timestamp, newest first (#1071).
        // Timestamps are ISO 8601, so string '>' is chronological.
        if (a_null && a.registered != b.registered)
            return a.registered > b.registered;
        return a.id > b.id; // tie-break when timestamps absent/equal
    });
}
