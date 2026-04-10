/*
 * PATTERN: String Matching (KMP, Rabin-Karp, Z-Algorithm)
 *
 * CONCEPT:
 * Naive string search is O(n*m). Three classic linear-time algorithms:
 * - KMP: Precompute failure function (longest proper prefix = suffix).
 *   On mismatch, jump using the table instead of restarting.
 * - Rabin-Karp: Rolling hash of the pattern; compare hashes first, then verify.
 *   Best for multiple-pattern or 2D search.
 * - Z-Algorithm: Z[i] = length of longest string starting from i that is also a
 *   prefix of the original string. Direct and elegant.
 *
 * TIME:  O(n + m) for all three
 * SPACE: O(m) for failure function / Z-array
 *
 * WHEN TO USE:
 * - "Find all occurrences of pattern in text"
 * - Repeated substring detection
 * - Longest prefix that is also a suffix (KMP failure table)
 * - Multiple patterns: extend to Aho-Corasick
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// KMP: Failure Function + Search
// ─────────────────────────────────────────────
vector<int> buildFailure(const string& p) {
    int m = (int)p.size();
    vector<int> fail(m, 0);
    for (int i = 1, j = 0; i < m; ) {
        if (p[i] == p[j]) { fail[i++] = ++j; }
        else if (j)         { j = fail[j-1]; }
        else                { fail[i++] = 0; }
    }
    return fail;
}

// ─────────────────────────────────────────────
// PROBLEM 1: KMP Search — All Occurrences  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Build failure table; scan text with two-pointer matching.
// Time: O(n + m)  Space: O(m)
vector<int> kmpSearch(const string& text, const string& pattern) {
    if (pattern.empty()) return {};
    auto fail = buildFailure(pattern);
    int n = (int)text.size(), m = (int)pattern.size();
    vector<int> result;
    for (int i = 0, j = 0; i < n; ) {
        if (text[i] == pattern[j]) { ++i; ++j; }
        if (j == m) { result.push_back(i - j); j = fail[j-1]; }
        else if (i < n && text[i] != pattern[j]) {
            if (j) j = fail[j-1]; else ++i;
        }
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Longest Prefix Suffix  [Difficulty: Easy]
// Source: LeetCode 1392 / Classic
// ─────────────────────────────────────────────
// Approach: KMP failure value at last position = length of longest prefix-suffix.
// Time: O(n)  Space: O(n)
string longestPrefixSuffix(const string& s) {
    auto fail = buildFailure(s);
    int len = fail.back();
    return s.substr(0, len);
}

// ─────────────────────────────────────────────
// Z-ALGORITHM
// ─────────────────────────────────────────────
vector<int> zFunction(const string& s) {
    int n = (int)s.size();
    vector<int> z(n, 0); z[0] = n;
    int l = 0, r = 0;
    for (int i = 1; i < n; ++i) {
        if (i < r) z[i] = min(r - i, z[i - l]);
        while (i + z[i] < n && s[z[i]] == s[i + z[i]]) ++z[i];
        if (i + z[i] > r) { l = i; r = i + z[i]; }
    }
    return z;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Z-Algorithm Search  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Concatenate pattern + '$' + text; z[i] == |pattern| means match.
// Time: O(n + m)  Space: O(n + m)
vector<int> zSearch(const string& text, const string& pattern) {
    string s = pattern + '$' + text;
    auto z = zFunction(s);
    int m = (int)pattern.size();
    vector<int> result;
    for (int i = m + 1; i < (int)s.size(); ++i)
        if (z[i] == m) result.push_back(i - m - 1);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Repeated Substring Pattern  [Difficulty: Easy]
// Source: LeetCode 459
// ─────────────────────────────────────────────
// Approach: If s is built by repeating a substring, failure[n-1] divides n
//           and n % (n - failure[n-1]) == 0.
// Time: O(n)  Space: O(n)
bool repeatedSubstringPattern(const string& s) {
    auto fail = buildFailure(s);
    int n = (int)s.size();
    int len = fail[n-1];
    return len > 0 && n % (n - len) == 0;
}

// ─────────────────────────────────────────────
// RABIN-KARP ROLLING HASH
// ─────────────────────────────────────────────
const long long BASE = 31, MOD = 1e9+9;

// ─────────────────────────────────────────────
// PROBLEM 5: Rabin-Karp Search  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Hash pattern; slide window hash over text; verify on hash match.
// Time: O(n + m) expected  Space: O(1)
vector<int> rabinKarpSearch(const string& text, const string& pattern) {
    int n = (int)text.size(), m = (int)pattern.size();
    if (m > n) return {};
    // Precompute pow(BASE, m-1) % MOD
    long long power = 1;
    for (int i = 0; i < m-1; ++i) power = power * BASE % MOD;
    long long patHash = 0, winHash = 0;
    for (int i = 0; i < m; ++i) {
        patHash = (patHash * BASE + pattern[i]) % MOD;
        winHash = (winHash * BASE + text[i]) % MOD;
    }
    vector<int> result;
    for (int i = 0; i <= n - m; ++i) {
        if (winHash == patHash && text.substr(i, m) == pattern)
            result.push_back(i);
        if (i < n - m)
            winHash = ((winHash - text[i] * power % MOD + MOD) * BASE + text[i+m]) % MOD;
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Longest Duplicate Substring  [Difficulty: Hard]
// Source: LeetCode 1044
// ─────────────────────────────────────────────
// Approach: Binary search on length; Rabin-Karp to check if any length-L dup exists.
// Time: O(n log n)  Space: O(n)
string longestDupSubstring(const string& s) {
    int n = (int)s.size();
    auto check = [&](int len) -> int {
        long long h = 0, power = 1;
        for (int i = 0; i < len; ++i) {
            h = (h * BASE + s[i]) % MOD;
            if (i < len-1) power = power * BASE % MOD;
        }
        unordered_map<long long, vector<int>> seen; seen[h].push_back(0);
        for (int i = 1; i + len <= n; ++i) {
            h = ((h - s[i-1] * power % MOD + MOD) * BASE + s[i+len-1]) % MOD;
            for (int prev : seen[h])
                if (s.substr(prev, len) == s.substr(i, len)) return i;
            seen[h].push_back(i);
        }
        return -1;
    };
    int lo = 1, hi = n-1, start = -1, bestLen = 0;
    while (lo <= hi) {
        int mid = lo + (hi-lo)/2;
        int pos = check(mid);
        if (pos != -1) { bestLen = mid; start = pos; lo = mid+1; }
        else hi = mid-1;
    }
    return start == -1 ? "" : s.substr(start, bestLen);
}

// ─────────────────────────────────────────────
// PROBLEM 7: Find All Anagrams in a String  [Difficulty: Medium]
// Source: LeetCode 438
// ─────────────────────────────────────────────
// Approach: Fixed-size sliding window with frequency comparison.
// Time: O(n)  Space: O(26)
vector<int> findAnagrams(const string& s, const string& p) {
    if (p.size() > s.size()) return {};
    vector<int> need(26,0), have(26,0);
    for (char c : p) ++need[c-'a'];
    int k = (int)p.size();
    for (int i = 0; i < k; ++i) ++have[s[i]-'a'];
    vector<int> result;
    if (have==need) result.push_back(0);
    for (int i = k; i < (int)s.size(); ++i) {
        ++have[s[i]-'a'];
        --have[s[i-k]-'a'];
        if (have==need) result.push_back(i-k+1);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Shortest Palindrome  [Difficulty: Hard]
// Source: LeetCode 214
// ─────────────────────────────────────────────
// Approach: KMP on s + '#' + reverse(s); failure value = longest palindrome prefix.
// Time: O(n)  Space: O(n)
string shortestPalindrome(const string& s) {
    string rev = s; reverse(rev.begin(), rev.end());
    string concat = s + '#' + rev;
    auto fail = buildFailure(concat);
    int palinLen = fail.back();
    return rev.substr(0, (int)s.size()-palinLen) + s;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Minimum Period of a String  [Difficulty: Medium]
// Source: Classic (Z-function or KMP)
// ─────────────────────────────────────────────
// Approach: Period = n - fail[n-1] if n % (n-fail[n-1]) == 0, else n.
// Time: O(n)  Space: O(n)
int minimumPeriod(const string& s) {
    auto fail = buildFailure(s);
    int n = (int)s.size();
    int period = n - fail[n-1];
    return (n % period == 0) ? period : n;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Is Rotation — Doubling Trick  [Difficulty: Easy]
// Source: LeetCode 796
// ─────────────────────────────────────────────
// Approach: s is rotation of goal iff goal appears in s+s.
// Time: O(n)  Space: O(n)
bool rotateString(const string& s, const string& goal) {
    if (s.size() != goal.size()) return false;
    return !kmpSearch(s + s, goal).empty();
}

// ─────────────────────────────────────────────
// PROBLEM 11: Number of Occurrences of a Substring  [Difficulty: Easy]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: KMP search, count matches.
// Time: O(n + m)  Space: O(m)
int countOccurrences(const string& text, const string& pattern) {
    return (int)kmpSearch(text, pattern).size();
}

// ─────────────────────────────────────────────
// PROBLEM 12: Aho-Corasick Multi-Pattern Search  [Difficulty: Hard]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Build trie + BFS failure links; scan text once to find all patterns.
// Time: O(sum of |patterns| + |text| + #matches)  Space: O(sum of |patterns| * 26)
struct AhoCorasick {
    struct Node {
        array<int,26> ch{};
        int fail = 0, output = -1; // output: pattern index or -1
        Node() { ch.fill(-1); }
    };
    vector<Node> nodes;
    AhoCorasick() { nodes.emplace_back(); }
    void insert(const string& s, int id) {
        int cur = 0;
        for (char c : s) {
            int idx = c-'a';
            if (nodes[cur].ch[idx] == -1) {
                nodes[cur].ch[idx] = (int)nodes.size();
                nodes.emplace_back();
            }
            cur = nodes[cur].ch[idx];
        }
        nodes[cur].output = id;
    }
    void build() {
        queue<int> q;
        for (int c = 0; c < 26; ++c) {
            if (nodes[0].ch[c] == -1) nodes[0].ch[c] = 0;
            else { nodes[nodes[0].ch[c]].fail = 0; q.push(nodes[0].ch[c]); }
        }
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int c = 0; c < 26; ++c) {
                if (nodes[u].ch[c] == -1) {
                    nodes[u].ch[c] = nodes[nodes[u].fail].ch[c];
                } else {
                    nodes[nodes[u].ch[c]].fail = nodes[nodes[u].fail].ch[c];
                    q.push(nodes[u].ch[c]);
                }
            }
        }
    }
    // Returns vector of (position, patternId)
    vector<pair<int,int>> search(const string& text) {
        vector<pair<int,int>> result;
        int cur = 0;
        for (int i = 0; i < (int)text.size(); ++i) {
            cur = nodes[cur].ch[text[i]-'a'];
            for (int tmp = cur; tmp; tmp = nodes[tmp].fail)
                if (nodes[tmp].output != -1) result.push_back({i, nodes[tmp].output});
        }
        return result;
    }
};

int main() {
    // Problem 1 — KMP
    auto p1 = kmpSearch("AABAACAADAABAABA", "AABA");
    for (int v : p1) cout << v << " "; cout << "\n"; // 0 9 12

    // Problem 2
    cout << longestPrefixSuffix("abacaba") << "\n"; // aba

    // Problem 3 — Z-search
    auto p3 = zSearch("AABAACAADAABAABA", "AABA");
    for (int v : p3) cout << v << " "; cout << "\n"; // 0 9 12

    // Problem 4
    cout << boolalpha << repeatedSubstringPattern("abcabcabcabc") << "\n"; // true
    cout << repeatedSubstringPattern("abac") << "\n"; // false

    // Problem 5 — Rabin-Karp
    auto p5 = rabinKarpSearch("AABAACAADAABAABA", "AABA");
    for (int v : p5) cout << v << " "; cout << "\n"; // 0 9 12

    // Problem 6
    cout << longestDupSubstring("banana") << "\n"; // ana

    // Problem 7
    auto p7 = findAnagrams("cbaebabacd", "abc");
    for (int v : p7) cout << v << " "; cout << "\n"; // 0 6

    // Problem 8
    cout << shortestPalindrome("aacecaaa") << "\n"; // aaacecaaa

    // Problem 9
    cout << minimumPeriod("abcabcabc") << "\n"; // 3

    // Problem 10
    cout << rotateString("abcde", "cdeab") << "\n"; // true

    // Problem 11
    cout << countOccurrences("AABAACAADAABAABA", "AABA") << "\n"; // 3

    // Problem 12 — Aho-Corasick
    AhoCorasick ac;
    ac.insert("he", 0); ac.insert("she", 1); ac.insert("his", 2); ac.insert("hers", 3);
    ac.build();
    auto matches = ac.search("ushers");
    for (auto& [pos, id] : matches) cout << "pattern " << id << " ends at " << pos << "\n";

    return 0;
}
