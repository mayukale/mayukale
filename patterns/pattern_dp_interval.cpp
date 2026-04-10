/*
 * PATTERN: Dynamic Programming — Interval (Range DP)
 *
 * CONCEPT:
 * dp[i][j] represents the optimal answer for a subarray/substring/subproblem
 * over the range [i, j]. Fill in increasing order of interval length.
 * The recurrence typically tries every split point k: i <= k < j.
 * Diagonal filling: start with length 1, then 2, etc.
 *
 * TIME:  O(n³) typical (n² intervals × n split points)
 * SPACE: O(n²)
 *
 * WHEN TO USE:
 * - "Minimum cost to split/merge a range"
 * - Matrix chain multiplication, burst balloons
 * - Palindrome count/partition over a range
 * - "Optimal BST", "minimum parenthesization"
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Matrix Chain Multiplication  [Difficulty: Hard]
// Source: Classic CLRS
// ─────────────────────────────────────────────
// Approach: dp[i][j] = min multiplications for matrices i..j; try all splits.
// Time: O(n³)  Space: O(n²)
int matrixChain(const vector<int>& dims) {
    int n = (int)dims.size() - 1;
    vector<vector<int>> dp(n, vector<int>(n, 0));
    for (int len = 2; len <= n; ++len) {
        for (int i = 0; i <= n - len; ++i) {
            int j = i + len - 1;
            dp[i][j] = INT_MAX;
            for (int k = i; k < j; ++k)
                dp[i][j] = min(dp[i][j], dp[i][k] + dp[k+1][j] + dims[i]*dims[k+1]*dims[j+1]);
        }
    }
    return dp[0][n-1];
}

// ─────────────────────────────────────────────
// PROBLEM 2: Burst Balloons  [Difficulty: Hard]
// Source: LeetCode 312
// ─────────────────────────────────────────────
// Approach: dp[i][j] = max coins for balloons in (i,j) exclusive;
//           k = last balloon burst in that range.
// Time: O(n³)  Space: O(n²)
int maxCoins(vector<int> nums) {
    nums.insert(nums.begin(), 1); nums.push_back(1);
    int n = (int)nums.size();
    vector<vector<int>> dp(n, vector<int>(n, 0));
    for (int len = 2; len < n; ++len) {
        for (int i = 0; i < n - len; ++i) {
            int j = i + len;
            for (int k = i+1; k < j; ++k)
                dp[i][j] = max(dp[i][j],
                    dp[i][k] + nums[i]*nums[k]*nums[j] + dp[k][j]);
        }
    }
    return dp[0][n-1];
}

// ─────────────────────────────────────────────
// PROBLEM 3: Palindrome Partitioning II (min cuts)  [Difficulty: Hard]
// Source: LeetCode 132
// ─────────────────────────────────────────────
// Approach: Precompute isPalin table; dp[i] = min cuts for s[0..i].
// Time: O(n²)  Space: O(n²)
int minCut(const string& s) {
    int n = (int)s.size();
    vector<vector<bool>> isPalin(n, vector<bool>(n, false));
    for (int i = n-1; i >= 0; --i)
        for (int j = i; j < n; ++j)
            isPalin[i][j] = (s[i]==s[j]) && (j-i<=2 || isPalin[i+1][j-1]);
    vector<int> dp(n, INT_MAX);
    for (int i = 0; i < n; ++i) {
        if (isPalin[0][i]) { dp[i] = 0; continue; }
        for (int j = 1; j <= i; ++j)
            if (isPalin[j][i]) dp[i] = min(dp[i], dp[j-1]+1);
    }
    return dp[n-1];
}

// ─────────────────────────────────────────────
// PROBLEM 4: Count Palindromic Substrings  [Difficulty: Medium]
// Source: LeetCode 647
// ─────────────────────────────────────────────
// Approach: Expand around each center (2n-1); count expansions.
// Time: O(n²)  Space: O(1)
int countSubstrings(const string& s) {
    int n = (int)s.size(), count = 0;
    for (int c = 0; c < 2*n - 1; ++c) {
        int l = c/2, r = c/2 + c%2;
        while (l >= 0 && r < n && s[l] == s[r]) { ++count; --l; ++r; }
    }
    return count;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Minimum Cost to Fill a Bag  [Difficulty: Medium]
// Source: Classic variant — Optimal interval split
// ─────────────────────────────────────────────
// Approach: dp[i][j] = min score to remove all stones in [i,j];
//           stones shrink as outer boundaries collapse.
// This is the "Stone Merging" problem.
// Time: O(n³)  Space: O(n²)
int stoneGameMin(const vector<int>& stones) {
    int n = (int)stones.size();
    vector<int> prefix(n+1, 0);
    for (int i = 0; i < n; ++i) prefix[i+1] = prefix[i] + stones[i];
    vector<vector<int>> dp(n, vector<int>(n, 0));
    for (int len = 2; len <= n; ++len) {
        for (int i = 0; i <= n-len; ++i) {
            int j = i+len-1;
            dp[i][j] = INT_MAX;
            for (int k = i; k < j; ++k)
                dp[i][j] = min(dp[i][j], dp[i][k]+dp[k+1][j]+(prefix[j+1]-prefix[i]));
        }
    }
    return dp[0][n-1];
}

// ─────────────────────────────────────────────
// PROBLEM 6: Unique BSTs (Catalan Number)  [Difficulty: Medium]
// Source: LeetCode 96
// ─────────────────────────────────────────────
// Approach: dp[n] = sum over all roots k: dp[k-1] * dp[n-k].
// Time: O(n²)  Space: O(n)
int numTrees(int n) {
    vector<int> dp(n+1, 0); dp[0] = dp[1] = 1;
    for (int i = 2; i <= n; ++i)
        for (int k = 1; k <= i; ++k) dp[i] += dp[k-1] * dp[i-k];
    return dp[n];
}

// ─────────────────────────────────────────────
// PROBLEM 7: Largest Sum of Averages  [Difficulty: Medium]
// Source: LeetCode 813
// ─────────────────────────────────────────────
// Approach: dp[k][i] = max avg sum splitting A[0..i-1] into k parts.
// Time: O(k*n²)  Space: O(k*n)
double largestSumOfAverages(const vector<int>& A, int k) {
    int n = (int)A.size();
    vector<double> prefix(n+1, 0);
    for (int i = 0; i < n; ++i) prefix[i+1] = prefix[i] + A[i];
    vector<vector<double>> dp(k+1, vector<double>(n+1, 0));
    // Base: 1 group = average of A[0..j-1]
    for (int j = 1; j <= n; ++j) dp[1][j] = prefix[j] / j;
    for (int p = 2; p <= k; ++p) {
        for (int j = p; j <= n; ++j) {
            for (int m = p-1; m < j; ++m)
                dp[p][j] = max(dp[p][j], dp[p-1][m] + (prefix[j]-prefix[m])/(j-m));
        }
    }
    return dp[k][n];
}

// ─────────────────────────────────────────────
// PROBLEM 8: Zuma Game  [Difficulty: Hard]
// Source: LeetCode 488
// ─────────────────────────────────────────────
// Approach: Interval DP with memoization on (board, hand) states.
// Time: O(n² * hand) memoized  Space: O(n * hand)
// Simplified version — minimum insertions to clear a board segment.
int findMinStep(string board, string hand) {
    sort(hand.begin(), hand.end());
    unordered_map<string, int> memo;
    function<int(string, string)> dp = [&](string b, string h) -> int {
        if (b.empty()) return 0;
        string key = b + "#" + h;
        if (memo.count(key)) return memo[key];
        // Eliminate consecutive groups >= 3
        string cleared = b;
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0, j = 0; i < (int)cleared.size(); ) {
                while (j < (int)cleared.size() && cleared[j] == cleared[i]) ++j;
                if (j - i >= 3) { cleared.erase(i, j-i); changed = true; }
                else i = j;
            }
        }
        int result = INT_MAX;
        for (int i = 0, j = 0; i < (int)cleared.size(); ) {
            while (j < (int)cleared.size() && cleared[j] == cleared[i]) ++j;
            int need = 3 - (j - i);
            string newHand = h;
            bool canInsert = true;
            for (int k = 0; k < need; ++k) {
                auto pos = newHand.find(cleared[i]);
                if (pos == string::npos) { canInsert = false; break; }
                newHand.erase(pos, 1);
            }
            if (canInsert) {
                string newBoard = cleared.substr(0, i) + cleared.substr(j);
                int sub = dp(newBoard, newHand);
                if (sub != INT_MAX) result = min(result, need + sub);
            }
            i = j;
        }
        return memo[key] = result;
    };
    int result = dp(board, hand);
    return result == INT_MAX ? -1 : result;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Strange Printer  [Difficulty: Hard]
// Source: LeetCode 664
// ─────────────────────────────────────────────
// Approach: dp[i][j] = min turns to print s[i..j];
//           if s[k]==s[j] for i<=k<j, we can merge turns.
// Time: O(n³)  Space: O(n²)
int strangePrinter(const string& s) {
    int n = (int)s.size();
    vector<vector<int>> dp(n, vector<int>(n, 0));
    for (int i = n-1; i >= 0; --i) {
        dp[i][i] = 1;
        for (int j = i+1; j < n; ++j) {
            dp[i][j] = dp[i][j-1] + 1; // worst case: print s[j] separately
            for (int k = i; k < j; ++k)
                if (s[k] == s[j])
                    dp[i][j] = min(dp[i][j], dp[i][k] + (k+1<=j-1 ? dp[k+1][j-1] : 0));
        }
    }
    return dp[0][n-1];
}

// ─────────────────────────────────────────────
// PROBLEM 10: Predict the Winner / Stone Game  [Difficulty: Medium]
// Source: LeetCode 486 / 877
// ─────────────────────────────────────────────
// Approach: dp[i][j] = max score difference (cur_player - other) for nums[i..j].
// Time: O(n²)  Space: O(n²)
bool predictTheWinner(const vector<int>& nums) {
    int n = (int)nums.size();
    vector<vector<int>> dp(n, vector<int>(n, 0));
    for (int i = 0; i < n; ++i) dp[i][i] = nums[i];
    for (int len = 2; len <= n; ++len)
        for (int i = 0; i <= n-len; ++i) {
            int j = i+len-1;
            dp[i][j] = max(nums[i] - dp[i+1][j], nums[j] - dp[i][j-1]);
        }
    return dp[0][n-1] >= 0;
}

int main() {
    // Problem 1
    cout << matrixChain({40,20,30,10,30}) << "\n"; // 26000

    // Problem 2
    cout << maxCoins({3,1,5,8}) << "\n"; // 167

    // Problem 3
    cout << minCut("aab") << "\n"; // 1

    // Problem 4
    cout << countSubstrings("abc") << "\n"; // 3

    // Problem 5
    cout << stoneGameMin({4,3,5,2}) << "\n"; // 26

    // Problem 6
    cout << numTrees(3) << "\n"; // 5

    // Problem 7
    cout << fixed << setprecision(5)
         << largestSumOfAverages({9,1,2,3,9}, 3) << "\n"; // 20.0

    // Problem 8 (simplified)
    cout << findMinStep("WRRBBW", "RB") << "\n"; // -1

    // Problem 9
    cout << strangePrinter("aaabbb") << "\n"; // 2

    // Problem 10
    cout << boolalpha << predictTheWinner({1,5,2}) << "\n"; // false
    cout << predictTheWinner({1,5,233,7}) << "\n"; // true

    return 0;
}
