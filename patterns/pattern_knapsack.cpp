/*
 * PATTERN: Knapsack (0/1 and Unbounded)
 *
 * CONCEPT:
 * 0/1 Knapsack: Each item can be taken at most once. Fill a DP table where
 * dp[w] = max value achievable with capacity w. Iterate items outer, capacity
 * inner (right-to-left to avoid reuse).
 * Unbounded Knapsack: Items can be reused. Inner loop goes left-to-right.
 * Both patterns generalize to "partition", "target sum", "coin change" etc.
 *
 * TIME:  O(n * W)
 * SPACE: O(W) with 1D rolling array
 *
 * WHEN TO USE:
 * - "Can we partition / fill to exactly target W?"
 * - "Max value within weight limit"
 * - "Minimum coins / items to reach sum"
 * - "Count ways to form target"
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: 0/1 Knapsack (classic)  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: 1D DP, iterate capacity right-to-left per item.
// Time: O(n*W)  Space: O(W)
int knapsack01(const vector<int>& weights, const vector<int>& values, int W) {
    vector<int> dp(W + 1, 0);
    for (int i = 0; i < (int)weights.size(); ++i)
        for (int w = W; w >= weights[i]; --w)
            dp[w] = max(dp[w], dp[w - weights[i]] + values[i]);
    return dp[W];
}

// ─────────────────────────────────────────────
// PROBLEM 2: Partition Equal Subset Sum  [Difficulty: Medium]
// Source: LeetCode 416
// ─────────────────────────────────────────────
// Approach: Target = sum/2; 0/1 knapsack feasibility.
// Time: O(n*sum)  Space: O(sum)
bool canPartition(const vector<int>& nums) {
    int total = accumulate(nums.begin(), nums.end(), 0);
    if (total % 2) return false;
    int target = total / 2;
    vector<bool> dp(target + 1, false);
    dp[0] = true;
    for (int n : nums)
        for (int w = target; w >= n; --w)
            dp[w] = dp[w] || dp[w - n];
    return dp[target];
}

// ─────────────────────────────────────────────
// PROBLEM 3: Subset Sum Count  [Difficulty: Medium]
// Source: Classic / LeetCode 494 variant
// ─────────────────────────────────────────────
// Approach: Count ways to form target using 0/1 selection.
// Time: O(n*sum)  Space: O(sum)
int countSubsetSum(const vector<int>& nums, int target) {
    vector<int> dp(target + 1, 0);
    dp[0] = 1;
    for (int n : nums)
        for (int w = target; w >= n; --w)
            dp[w] += dp[w - n];
    return dp[target];
}

// ─────────────────────────────────────────────
// PROBLEM 4: Target Sum (assign +/-)  [Difficulty: Medium]
// Source: LeetCode 494
// ─────────────────────────────────────────────
// Approach: Reduce to count-subset-sum: P - N = target, P + N = sum => P = (sum+target)/2.
// Time: O(n*sum)  Space: O(sum)
int findTargetSumWays(const vector<int>& nums, int target) {
    int total = accumulate(nums.begin(), nums.end(), 0);
    int S = total + target;
    if (S < 0 || S % 2) return 0;
    int t = S / 2;
    vector<int> dp(t + 1, 0);
    dp[0] = 1;
    for (int n : nums)
        for (int w = t; w >= n; --w)
            dp[w] += dp[w - n];
    return dp[t];
}

// ─────────────────────────────────────────────
// PROBLEM 5: Minimum Subset Sum Difference  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Fill DP up to sum/2; largest reachable sum s gives diff = total - 2*s.
// Time: O(n*sum)  Space: O(sum)
int minimumDifference(const vector<int>& nums) {
    int total = accumulate(nums.begin(), nums.end(), 0);
    int half = total / 2;
    vector<bool> dp(half + 1, false);
    dp[0] = true;
    for (int n : nums)
        for (int w = half; w >= n; --w)
            dp[w] = dp[w] || dp[w - n];
    for (int s = half; s >= 0; --s)
        if (dp[s]) return total - 2 * s;
    return total;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Coin Change (min coins)  [Difficulty: Medium]
// Source: LeetCode 322
// ─────────────────────────────────────────────
// Approach: Unbounded knapsack; dp[w] = min coins for amount w; left-to-right.
// Time: O(amount * n)  Space: O(amount)
int coinChange(const vector<int>& coins, int amount) {
    vector<int> dp(amount + 1, INT_MAX);
    dp[0] = 0;
    for (int c : coins)
        for (int w = c; w <= amount; ++w)
            if (dp[w - c] != INT_MAX) dp[w] = min(dp[w], dp[w - c] + 1);
    return dp[amount] == INT_MAX ? -1 : dp[amount];
}

// ─────────────────────────────────────────────
// PROBLEM 7: Coin Change II (count ways)  [Difficulty: Medium]
// Source: LeetCode 518
// ─────────────────────────────────────────────
// Approach: Unbounded; count combinations: iterate coin outer, amount inner.
// Time: O(amount * n)  Space: O(amount)
int change(int amount, const vector<int>& coins) {
    vector<int> dp(amount + 1, 0);
    dp[0] = 1;
    for (int c : coins)
        for (int w = c; w <= amount; ++w)
            dp[w] += dp[w - c];
    return dp[amount];
}

// ─────────────────────────────────────────────
// PROBLEM 8: Unbounded Knapsack (max value, unlimited items)  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Iterate capacity left-to-right per item (reuse allowed).
// Time: O(n*W)  Space: O(W)
int unboundedKnapsack(const vector<int>& weights, const vector<int>& values, int W) {
    vector<int> dp(W + 1, 0);
    for (int w = 1; w <= W; ++w)
        for (int i = 0; i < (int)weights.size(); ++i)
            if (weights[i] <= w) dp[w] = max(dp[w], dp[w - weights[i]] + values[i]);
    return dp[W];
}

// ─────────────────────────────────────────────
// PROBLEM 9: Rod Cutting  [Difficulty: Medium]
// Source: Classic / CLRS
// ─────────────────────────────────────────────
// Approach: Unbounded knapsack; item = piece length 1..n; value = price[i].
// Time: O(n²)  Space: O(n)
int rodCutting(const vector<int>& prices, int n) {
    vector<int> dp(n + 1, 0);
    for (int len = 1; len <= n; ++len)
        for (int cut = 1; cut <= len; ++cut)
            dp[len] = max(dp[len], prices[cut - 1] + dp[len - cut]);
    return dp[n];
}

// ─────────────────────────────────────────────
// PROBLEM 10: Last Stone Weight II  [Difficulty: Medium]
// Source: LeetCode 1049
// ─────────────────────────────────────────────
// Approach: Same as minimum subset sum difference; partition into two groups.
// Time: O(n*sum)  Space: O(sum)
int lastStoneWeightII(const vector<int>& stones) {
    return minimumDifference(stones);
}

// ─────────────────────────────────────────────
// PROBLEM 11: Perfect Squares  [Difficulty: Medium]
// Source: LeetCode 279
// ─────────────────────────────────────────────
// Approach: Unbounded knapsack; coins = perfect squares <= n.
// Time: O(n * sqrt(n))  Space: O(n)
int numSquares(int n) {
    vector<int> dp(n + 1, INT_MAX);
    dp[0] = 0;
    for (int i = 1; i * i <= n; ++i)
        for (int w = i * i; w <= n; ++w)
            if (dp[w - i*i] != INT_MAX) dp[w] = min(dp[w], dp[w - i*i] + 1);
    return dp[n];
}

int main() {
    // Problem 1
    cout << knapsack01({1,3,4,5}, {1,4,5,7}, 7) << "\n"; // 9

    // Problem 2
    cout << boolalpha << canPartition({1,5,11,5}) << "\n"; // true
    cout << canPartition({1,2,3,5}) << "\n"; // false

    // Problem 3
    cout << countSubsetSum({1,1,2,3}, 4) << "\n"; // 3

    // Problem 4
    cout << findTargetSumWays({1,1,1,1,1}, 3) << "\n"; // 5

    // Problem 5
    cout << minimumDifference({1,6,11,5}) << "\n"; // 1

    // Problem 6
    cout << coinChange({1,5,6,9}, 11) << "\n"; // 2

    // Problem 7
    cout << change(5, {1,2,5}) << "\n"; // 4

    // Problem 8
    cout << unboundedKnapsack({2,3,4,5}, {6,10,7,8}, 5) << "\n"; // 12

    // Problem 9
    cout << rodCutting({1,5,8,9,10,17,17,20}, 8) << "\n"; // 22

    // Problem 10
    cout << lastStoneWeightII({2,7,4,1,8,1}) << "\n"; // 1

    // Problem 11
    cout << numSquares(12) << "\n"; // 3

    return 0;
}
