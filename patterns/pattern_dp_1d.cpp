/*
 * PATTERN: Dynamic Programming — 1D
 *
 * CONCEPT:
 * Build a 1D array dp[] where dp[i] represents the optimal answer for a
 * subproblem of size i. Each entry depends only on previous entries.
 * Rolling variable optimization reduces space to O(1) when dp[i] depends
 * on only O(1) previous states.
 *
 * TIME:  O(n) to O(n²)
 * SPACE: O(n) or O(1) with rolling variables
 *
 * WHEN TO USE:
 * - "Maximum/minimum/count for size n"
 * - Fibonacci-like recurrences
 * - Climbing stairs, house robber, decode ways
 * - "Breaking" problems: word break, palindrome partition
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Climbing Stairs  [Difficulty: Easy]
// Source: LeetCode 70
// ─────────────────────────────────────────────
// Approach: dp[i] = dp[i-1] + dp[i-2] (Fibonacci).
// Time: O(n)  Space: O(1)
int climbStairs(int n) {
    if (n <= 2) return n;
    int a = 1, b = 2;
    for (int i = 3; i <= n; ++i) { int c = a + b; a = b; b = c; }
    return b;
}

// ─────────────────────────────────────────────
// PROBLEM 2: House Robber  [Difficulty: Medium]
// Source: LeetCode 198
// ─────────────────────────────────────────────
// Approach: dp[i] = max(dp[i-1], dp[i-2] + nums[i]).
// Time: O(n)  Space: O(1)
int rob(const vector<int>& nums) {
    int prev2 = 0, prev1 = 0;
    for (int n : nums) {
        int cur = max(prev1, prev2 + n);
        prev2 = prev1; prev1 = cur;
    }
    return prev1;
}

// ─────────────────────────────────────────────
// PROBLEM 3: House Robber II (circular)  [Difficulty: Medium]
// Source: LeetCode 213
// ─────────────────────────────────────────────
// Approach: max(rob[0..n-2], rob[1..n-1]).
// Time: O(n)  Space: O(1)
int robRange(const vector<int>& nums, int l, int r) {
    int prev2 = 0, prev1 = 0;
    for (int i = l; i <= r; ++i) {
        int cur = max(prev1, prev2 + nums[i]);
        prev2 = prev1; prev1 = cur;
    }
    return prev1;
}
int robII(const vector<int>& nums) {
    int n = (int)nums.size();
    if (n == 1) return nums[0];
    return max(robRange(nums, 0, n-2), robRange(nums, 1, n-1));
}

// ─────────────────────────────────────────────
// PROBLEM 4: Maximum Product Subarray  [Difficulty: Medium]
// Source: LeetCode 152
// ─────────────────────────────────────────────
// Approach: Track both max and min product ending here (negatives flip them).
// Time: O(n)  Space: O(1)
int maxProduct(const vector<int>& nums) {
    int maxP = nums[0], minP = nums[0], result = nums[0];
    for (int i = 1; i < (int)nums.size(); ++i) {
        if (nums[i] < 0) swap(maxP, minP);
        maxP = max(nums[i], maxP * nums[i]);
        minP = min(nums[i], minP * nums[i]);
        result = max(result, maxP);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Longest Increasing Subsequence  [Difficulty: Medium]
// Source: LeetCode 300
// ─────────────────────────────────────────────
// Approach (O(n log n)): Patience sorting — maintain tails array; binary search.
// Time: O(n log n)  Space: O(n)
int lengthOfLIS(const vector<int>& nums) {
    vector<int> tails;
    for (int n : nums) {
        auto it = lower_bound(tails.begin(), tails.end(), n);
        if (it == tails.end()) tails.push_back(n);
        else *it = n;
    }
    return (int)tails.size();
}

// ─────────────────────────────────────────────
// PROBLEM 6: Decode Ways  [Difficulty: Medium]
// Source: LeetCode 91
// ─────────────────────────────────────────────
// Approach: dp[i] = ways to decode s[0..i-1]; consider 1 and 2-digit endings.
// Time: O(n)  Space: O(1)
int numDecodings(const string& s) {
    if (s[0] == '0') return 0;
    int n = (int)s.size(), dp1 = 1, dp2 = 1;
    for (int i = 1; i < n; ++i) {
        int cur = 0;
        if (s[i] != '0') cur = dp1;
        int two = stoi(s.substr(i-1, 2));
        if (two >= 10 && two <= 26) cur += dp2;
        dp2 = dp1; dp1 = cur;
    }
    return dp1;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Word Break  [Difficulty: Medium]
// Source: LeetCode 139
// ─────────────────────────────────────────────
// Approach: dp[i] = can s[0..i-1] be segmented; check all word endings at i.
// Time: O(n² * m)  Space: O(n)
bool wordBreak(const string& s, const vector<string>& wordDict) {
    unordered_set<string> dict(wordDict.begin(), wordDict.end());
    int n = (int)s.size();
    vector<bool> dp(n+1, false);
    dp[0] = true;
    for (int i = 1; i <= n; ++i)
        for (int j = 0; j < i; ++j)
            if (dp[j] && dict.count(s.substr(j, i-j))) { dp[i] = true; break; }
    return dp[n];
}

// ─────────────────────────────────────────────
// PROBLEM 8: Minimum Cost for Tickets  [Difficulty: Medium]
// Source: LeetCode 983
// ─────────────────────────────────────────────
// Approach: dp[i] = min cost to travel up to day i; check travel days.
// Time: O(365)  Space: O(365)
int mincostTickets(const vector<int>& days, const vector<int>& costs) {
    unordered_set<int> travelDays(days.begin(), days.end());
    vector<int> dp(366, 0);
    for (int i = 1; i <= 365; ++i) {
        if (!travelDays.count(i)) { dp[i] = dp[i-1]; continue; }
        dp[i] = min({dp[i-1]   + costs[0],
                     dp[max(0,i-7)]  + costs[1],
                     dp[max(0,i-30)] + costs[2]});
    }
    return dp[365];
}

// ─────────────────────────────────────────────
// PROBLEM 9: Maximum Sum Circular Subarray  [Difficulty: Medium]
// Source: LeetCode 918
// ─────────────────────────────────────────────
// Approach: max(Kadane, total - minSubarray); handle all-negative edge case.
// Time: O(n)  Space: O(1)
int maxSubarraySumCircular(const vector<int>& nums) {
    int total = 0, maxSum = nums[0], curMax = 0;
    int minSum = nums[0], curMin = 0;
    for (int n : nums) {
        curMax = max(n, curMax + n); maxSum = max(maxSum, curMax);
        curMin = min(n, curMin + n); minSum = min(minSum, curMin);
        total += n;
    }
    return maxSum > 0 ? max(maxSum, total - minSum) : maxSum;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Longest Turbulent Subarray  [Difficulty: Medium]
// Source: LeetCode 978
// ─────────────────────────────────────────────
// Approach: Track inc/dec streak lengths; reset on equal.
// Time: O(n)  Space: O(1)
int maxTurbulenceSize(const vector<int>& arr) {
    int n = (int)arr.size(), maxLen = 1, inc = 1, dec = 1;
    for (int i = 1; i < n; ++i) {
        if (arr[i] > arr[i-1]) { inc = dec + 1; dec = 1; }
        else if (arr[i] < arr[i-1]) { dec = inc + 1; inc = 1; }
        else { inc = dec = 1; }
        maxLen = max(maxLen, max(inc, dec));
    }
    return maxLen;
}

// ─────────────────────────────────────────────
// PROBLEM 11: Number of Longest Increasing Subsequences  [Difficulty: Medium]
// Source: LeetCode 673
// ─────────────────────────────────────────────
// Approach: dp[i] = (length, count); scan all j<i to extend.
// Time: O(n²)  Space: O(n)
int findNumberOfLIS(const vector<int>& nums) {
    int n = (int)nums.size(), maxLen = 0, result = 0;
    vector<int> len(n, 1), cnt(n, 1);
    for (int i = 1; i < n; ++i) {
        for (int j = 0; j < i; ++j) {
            if (nums[j] < nums[i]) {
                if (len[j]+1 > len[i]) { len[i] = len[j]+1; cnt[i] = cnt[j]; }
                else if (len[j]+1 == len[i]) cnt[i] += cnt[j];
            }
        }
        maxLen = max(maxLen, len[i]);
    }
    for (int i = 0; i < n; ++i) if (len[i] == maxLen) result += cnt[i];
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 12: Wiggle Subsequence  [Difficulty: Medium]
// Source: LeetCode 376
// ─────────────────────────────────────────────
// Approach: Greedy DP; track last direction; count peaks/valleys.
// Time: O(n)  Space: O(1)
int wiggleMaxLength(const vector<int>& nums) {
    if (nums.size() < 2) return (int)nums.size();
    int up = 1, down = 1;
    for (int i = 1; i < (int)nums.size(); ++i) {
        if (nums[i] > nums[i-1]) up = down + 1;
        else if (nums[i] < nums[i-1]) down = up + 1;
    }
    return max(up, down);
}

int main() {
    cout << climbStairs(5) << "\n"; // 8
    cout << rob({2,7,9,3,1}) << "\n"; // 12
    cout << robII({2,3,2}) << "\n"; // 3
    cout << maxProduct({2,3,-2,4}) << "\n"; // 6
    cout << lengthOfLIS({10,9,2,5,3,7,101,18}) << "\n"; // 4
    cout << numDecodings("226") << "\n"; // 3
    cout << boolalpha << wordBreak("leetcode", {"leet","code"}) << "\n"; // true
    cout << mincostTickets({1,4,6,7,8,20}, {2,7,15}) << "\n"; // 11
    cout << maxSubarraySumCircular({5,-3,5}) << "\n"; // 10
    cout << maxTurbulenceSize({9,4,2,10,7,8,8,1,9}) << "\n"; // 5
    cout << findNumberOfLIS({1,3,5,4,7}) << "\n"; // 2
    cout << wiggleMaxLength({1,7,4,9,2,5}) << "\n"; // 6
    return 0;
}
