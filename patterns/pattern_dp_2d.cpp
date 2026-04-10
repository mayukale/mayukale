/*
 * PATTERN: Dynamic Programming — 2D
 *
 * CONCEPT:
 * Build a 2D table dp[i][j] representing the optimal answer for subproblems
 * parameterized by two dimensions (e.g., string prefixes, grid coordinates).
 * Fill row-by-row or diagonally depending on dependencies. Space can often
 * be compressed to O(n) using a rolling row.
 *
 * TIME:  O(m*n) typical
 * SPACE: O(m*n) or O(n) compressed
 *
 * WHEN TO USE:
 * - Two sequences: LCS, edit distance, regex matching
 * - Grid DP: unique paths, minimum path sum, triangle
 * - String DP: longest palindromic substring, distinct subsequences
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Unique Paths  [Difficulty: Medium]
// Source: LeetCode 62
// ─────────────────────────────────────────────
// Approach: dp[i][j] = dp[i-1][j] + dp[i][j-1]; compress to 1D.
// Time: O(m*n)  Space: O(n)
int uniquePaths(int m, int n) {
    vector<int> dp(n, 1);
    for (int i = 1; i < m; ++i) for (int j = 1; j < n; ++j) dp[j] += dp[j-1];
    return dp[n-1];
}

// ─────────────────────────────────────────────
// PROBLEM 2: Minimum Path Sum  [Difficulty: Medium]
// Source: LeetCode 64
// ─────────────────────────────────────────────
// Approach: dp[i][j] = grid[i][j] + min(dp[i-1][j], dp[i][j-1]); in-place.
// Time: O(m*n)  Space: O(1)
int minPathSum(vector<vector<int>> grid) {
    int m = (int)grid.size(), n = (int)grid[0].size();
    for (int i = 0; i < m; ++i) for (int j = 0; j < n; ++j) {
        if (!i && !j) continue;
        int up   = i > 0 ? grid[i-1][j] : INT_MAX;
        int left = j > 0 ? grid[i][j-1] : INT_MAX;
        grid[i][j] += min(up, left);
    }
    return grid[m-1][n-1];
}

// ─────────────────────────────────────────────
// PROBLEM 3: Longest Common Subsequence  [Difficulty: Medium]
// Source: LeetCode 1143
// ─────────────────────────────────────────────
// Approach: dp[i][j] = LCS of text1[0..i-1] and text2[0..j-1].
// Time: O(m*n)  Space: O(m*n) — can be O(n)
int longestCommonSubsequence(const string& text1, const string& text2) {
    int m = (int)text1.size(), n = (int)text2.size();
    vector<vector<int>> dp(m+1, vector<int>(n+1, 0));
    for (int i = 1; i <= m; ++i) for (int j = 1; j <= n; ++j)
        dp[i][j] = (text1[i-1]==text2[j-1]) ? dp[i-1][j-1]+1
                                              : max(dp[i-1][j], dp[i][j-1]);
    return dp[m][n];
}

// ─────────────────────────────────────────────
// PROBLEM 4: Edit Distance (Levenshtein)  [Difficulty: Hard]
// Source: LeetCode 72
// ─────────────────────────────────────────────
// Approach: dp[i][j] = min edit ops for word1[0..i-1] → word2[0..j-1].
// Time: O(m*n)  Space: O(n)
int minDistance(const string& word1, const string& word2) {
    int m = (int)word1.size(), n = (int)word2.size();
    vector<int> dp(n+1);
    iota(dp.begin(), dp.end(), 0);
    for (int i = 1; i <= m; ++i) {
        int prev = dp[0]; dp[0] = i;
        for (int j = 1; j <= n; ++j) {
            int tmp = dp[j];
            dp[j] = (word1[i-1]==word2[j-1]) ? prev
                  : 1 + min({prev, dp[j], dp[j-1]});
            prev = tmp;
        }
    }
    return dp[n];
}

// ─────────────────────────────────────────────
// PROBLEM 5: Longest Palindromic Substring  [Difficulty: Medium]
// Source: LeetCode 5
// ─────────────────────────────────────────────
// Approach: Expand around centers (2n-1 centers); O(n) space.
// Time: O(n²)  Space: O(1)
string longestPalindrome(const string& s) {
    int n = (int)s.size(), start = 0, maxLen = 0;
    auto expand = [&](int l, int r) {
        while (l >= 0 && r < n && s[l] == s[r]) { --l; ++r; }
        if (r - l - 1 > maxLen) { maxLen = r-l-1; start = l+1; }
    };
    for (int i = 0; i < n; ++i) { expand(i, i); expand(i, i+1); }
    return s.substr(start, maxLen);
}

// ─────────────────────────────────────────────
// PROBLEM 6: Distinct Subsequences  [Difficulty: Hard]
// Source: LeetCode 115
// ─────────────────────────────────────────────
// Approach: dp[i][j] = # ways t[0..j-1] appears in s[0..i-1].
// Time: O(m*n)  Space: O(n)
int numDistinct(const string& s, const string& t) {
    int m = (int)s.size(), n = (int)t.size();
    vector<long long> dp(n+1, 0); dp[0] = 1;
    for (int i = 1; i <= m; ++i)
        for (int j = n; j >= 1; --j)
            if (s[i-1] == t[j-1]) dp[j] += dp[j-1];
    return (int)dp[n];
}

// ─────────────────────────────────────────────
// PROBLEM 7: Interleaving String  [Difficulty: Medium]
// Source: LeetCode 97
// ─────────────────────────────────────────────
// Approach: dp[i][j] = s1[0..i-1] and s2[0..j-1] interleave to s3[0..i+j-1].
// Time: O(m*n)  Space: O(n)
bool isInterleave(const string& s1, const string& s2, const string& s3) {
    int m = (int)s1.size(), n = (int)s2.size();
    if (m+n != (int)s3.size()) return false;
    vector<bool> dp(n+1, false); dp[0] = true;
    for (int j = 1; j <= n; ++j) dp[j] = dp[j-1] && s2[j-1]==s3[j-1];
    for (int i = 1; i <= m; ++i) {
        dp[0] = dp[0] && s1[i-1]==s3[i-1];
        for (int j = 1; j <= n; ++j)
            dp[j] = (dp[j]   && s1[i-1]==s3[i+j-1]) ||
                    (dp[j-1] && s2[j-1]==s3[i+j-1]);
    }
    return dp[n];
}

// ─────────────────────────────────────────────
// PROBLEM 8: Maximal Square  [Difficulty: Medium]
// Source: LeetCode 221
// ─────────────────────────────────────────────
// Approach: dp[i][j] = side of largest square with bottom-right at (i,j).
// Time: O(m*n)  Space: O(n)
int maximalSquare(const vector<vector<char>>& matrix) {
    int m = (int)matrix.size(), n = (int)matrix[0].size(), maxSide = 0;
    vector<int> dp(n+1, 0);
    for (int i = 1; i <= m; ++i) {
        int prev = 0;
        for (int j = 1; j <= n; ++j) {
            int tmp = dp[j];
            dp[j] = (matrix[i-1][j-1]=='0') ? 0 : min({dp[j], dp[j-1], prev}) + 1;
            prev = tmp;
            maxSide = max(maxSide, dp[j]);
        }
    }
    return maxSide * maxSide;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Dungeon Game  [Difficulty: Hard]
// Source: LeetCode 174
// ─────────────────────────────────────────────
// Approach: Fill DP bottom-right to top-left; dp[i][j] = min health needed.
// Time: O(m*n)  Space: O(n)
int calculateMinimumHP(const vector<vector<int>>& dungeon) {
    int m = (int)dungeon.size(), n = (int)dungeon[0].size();
    vector<int> dp(n+1, INT_MAX); dp[n-1] = 1;
    for (int i = m-1; i >= 0; --i) {
        vector<int> next(n+1, INT_MAX);
        for (int j = n-1; j >= 0; --j) {
            int need = min(dp[j], (j+1<=n-1?dp[j+1]:INT_MAX)) - dungeon[i][j];
            next[j] = max(1, need);
        }
        dp = next;
    }
    return dp[0];
}

// ─────────────────────────────────────────────
// PROBLEM 10: Minimum ASCII Delete Sum  [Difficulty: Medium]
// Source: LeetCode 712
// ─────────────────────────────────────────────
// Approach: Like LCS but sum ASCII costs; dp[i][j] = min delete cost.
// Time: O(m*n)  Space: O(m*n)
int minimumDeleteSum(const string& s1, const string& s2) {
    int m = (int)s1.size(), n = (int)s2.size();
    vector<vector<int>> dp(m+1, vector<int>(n+1, 0));
    for (int i = 1; i <= m; ++i) dp[i][0] = dp[i-1][0] + s1[i-1];
    for (int j = 1; j <= n; ++j) dp[0][j] = dp[0][j-1] + s2[j-1];
    for (int i = 1; i <= m; ++i) for (int j = 1; j <= n; ++j)
        dp[i][j] = (s1[i-1]==s2[j-1]) ? dp[i-1][j-1]
                 : min(dp[i-1][j]+s1[i-1], dp[i][j-1]+s2[j-1]);
    return dp[m][n];
}

// ─────────────────────────────────────────────
// PROBLEM 11: Longest Common Substring (vs Subsequence)  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: dp[i][j] = length of common substring ending at s1[i-1] and s2[j-1].
// Time: O(m*n)  Space: O(n)
int longestCommonSubstring(const string& s1, const string& s2) {
    int m = (int)s1.size(), n = (int)s2.size(), maxLen = 0;
    vector<int> dp(n+1, 0);
    for (int i = 1; i <= m; ++i) {
        vector<int> prev = dp;
        for (int j = 1; j <= n; ++j)
            dp[j] = (s1[i-1]==s2[j-1]) ? prev[j-1]+1 : 0;
        maxLen = max(maxLen, *max_element(dp.begin(), dp.end()));
    }
    return maxLen;
}

int main() {
    cout << uniquePaths(3, 7) << "\n"; // 28
    cout << minPathSum({{1,3,1},{1,5,1},{4,2,1}}) << "\n"; // 7
    cout << longestCommonSubsequence("abcde","ace") << "\n"; // 3
    cout << minDistance("horse","ros") << "\n"; // 3
    cout << longestPalindrome("babad") << "\n"; // bab
    cout << numDistinct("rabbbit","rabbit") << "\n"; // 3
    cout << boolalpha << isInterleave("aabcc","dbbca","aadbbcbcac") << "\n"; // true
    cout << maximalSquare({{'1','0','1','0','0'},
                           {'1','0','1','1','1'},
                           {'1','1','1','1','1'},
                           {'1','0','0','1','0'}}) << "\n"; // 4
    cout << calculateMinimumHP({{-2,-3,3},{-5,-10,1},{10,30,-5}}) << "\n"; // 7
    cout << minimumDeleteSum("sea","eat") << "\n"; // 231
    cout << longestCommonSubstring("abcde","abce") << "\n"; // 3
    return 0;
}
