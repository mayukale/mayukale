/*
 * PATTERN: Bitwise XOR
 *
 * CONCEPT:
 * XOR is its own inverse: a ^ a = 0 and a ^ 0 = a. XOR-ing all elements in
 * a set where every number appears an even number of times leaves only the
 * odd-occurrence elements. Also useful for swapping without a temp variable,
 * finding set bits, and bit manipulation tricks.
 *
 * TIME:  O(n) for most XOR-scan problems
 * SPACE: O(1)
 *
 * WHEN TO USE:
 * - "Find the one number that appears once / odd times"
 * - "Find two missing / non-duplicate numbers"
 * - Bitmask operations, checking/toggling specific bits
 * - In-place swap, complement, power-of-two checks
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Single Number  [Difficulty: Easy]
// Source: LeetCode 136
// ─────────────────────────────────────────────
// Approach: XOR all elements; pairs cancel out, leaving the single.
// Time: O(n)  Space: O(1)
int singleNumber(const vector<int>& nums) {
    int result = 0;
    for (int n : nums) result ^= n;
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Single Number III (two singles)  [Difficulty: Medium]
// Source: LeetCode 260
// ─────────────────────────────────────────────
// Approach: XOR all -> xor of two singles. Find rightmost set bit to split into two groups.
// Time: O(n)  Space: O(1)
vector<int> singleNumberIII(const vector<int>& nums) {
    int xorAll = 0;
    for (int n : nums) xorAll ^= n;
    // Rightmost set bit differentiates the two singles
    int bit = xorAll & (-xorAll);
    int a = 0, b = 0;
    for (int n : nums) {
        if (n & bit) a ^= n;
        else         b ^= n;
    }
    return {a, b};
}

// ─────────────────────────────────────────────
// PROBLEM 3: Missing Number  [Difficulty: Easy]
// Source: LeetCode 268
// ─────────────────────────────────────────────
// Approach: XOR indices 0..n with all elements; missing index remains.
// Time: O(n)  Space: O(1)
int missingNumber(const vector<int>& nums) {
    int result = (int)nums.size();
    for (int i = 0; i < (int)nums.size(); ++i)
        result ^= i ^ nums[i];
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Find the Duplicate and Missing  [Difficulty: Medium]
// Source: Classic / LeetCode 645 variant
// ─────────────────────────────────────────────
// Approach: XOR with 1..n; find bit that separates; two-pass grouping.
// Time: O(n)  Space: O(1)
// Returns {missing, duplicate}
vector<int> findMissingAndDuplicate(const vector<int>& nums) {
    int n = (int)nums.size();
    int xorAll = 0;
    for (int i = 1; i <= n; ++i) xorAll ^= i;
    for (int v : nums) xorAll ^= v;
    // xorAll = missing XOR duplicate
    int bit = xorAll & (-xorAll);
    int g1 = 0, g2 = 0;
    for (int i = 1; i <= n; ++i) {
        if (i & bit) g1 ^= i; else g2 ^= i;
    }
    for (int v : nums) {
        if (v & bit) g1 ^= v; else g2 ^= v;
    }
    // Determine which is missing and which is duplicate
    for (int v : nums) if (v == g1) return {g2, g1};
    return {g1, g2};
}

// ─────────────────────────────────────────────
// PROBLEM 5: Complement of Base 10 Number  [Difficulty: Easy]
// Source: LeetCode 1009
// ─────────────────────────────────────────────
// Approach: XOR with a mask of all 1s of same bit-length.
// Time: O(log n)  Space: O(1)
int bitwiseComplement(int n) {
    if (n == 0) return 1;
    int mask = 1;
    while (mask <= n) mask <<= 1;
    return (mask - 1) ^ n;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Flip and Invert Image  [Difficulty: Easy]
// Source: LeetCode 832
// ─────────────────────────────────────────────
// Approach: Two-pointer reverse + XOR 1 to invert simultaneously.
// Time: O(n²)  Space: O(1)
vector<vector<int>> flipAndInvertImage(vector<vector<int>> image) {
    for (auto& row : image) {
        int l = 0, r = (int)row.size() - 1;
        while (l <= r) {
            swap(row[l], row[r]);
            row[l] ^= 1; if (l != r) row[r] ^= 1;
            ++l; --r;
        }
    }
    return image;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Power of Two  [Difficulty: Easy]
// Source: LeetCode 231
// ─────────────────────────────────────────────
// Approach: n & (n-1) clears the lowest set bit; power of 2 has exactly one.
// Time: O(1)  Space: O(1)
bool isPowerOfTwo(int n) { return n > 0 && (n & (n - 1)) == 0; }

// ─────────────────────────────────────────────
// PROBLEM 8: Counting Bits  [Difficulty: Easy]
// Source: LeetCode 338
// ─────────────────────────────────────────────
// Approach: popcount[i] = popcount[i>>1] + (i & 1).
// Time: O(n)  Space: O(n)
vector<int> countBits(int n) {
    vector<int> dp(n + 1, 0);
    for (int i = 1; i <= n; ++i) dp[i] = dp[i >> 1] + (i & 1);
    return dp;
}

// ─────────────────────────────────────────────
// PROBLEM 9: XOR Queries of a Subarray  [Difficulty: Medium]
// Source: LeetCode 1310
// ─────────────────────────────────────────────
// Approach: Build prefix-XOR; query [l,r] = prefix[r+1] ^ prefix[l].
// Time: O(n + q)  Space: O(n)
vector<int> xorQueries(const vector<int>& arr, const vector<vector<int>>& queries) {
    int n = (int)arr.size();
    vector<int> prefix(n + 1, 0);
    for (int i = 0; i < n; ++i) prefix[i+1] = prefix[i] ^ arr[i];
    vector<int> result;
    for (auto& q : queries) result.push_back(prefix[q[1]+1] ^ prefix[q[0]]);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Minimum XOR Sum of Two Arrays  [Difficulty: Hard]
// Source: LeetCode 1879 (DP + bitmask — here bitmask enumeration via XOR)
// ─────────────────────────────────────────────
// Approach: Bitmask DP; dp[mask] = min XOR sum assigning first popcount(mask) of nums2 to nums1.
// Time: O(n * 2^n)  Space: O(2^n)
int minimumXORSum(const vector<int>& nums1, const vector<int>& nums2) {
    int n = (int)nums1.size();
    vector<int> dp(1 << n, INT_MAX);
    dp[0] = 0;
    for (int mask = 0; mask < (1 << n); ++mask) {
        if (dp[mask] == INT_MAX) continue;
        int i = __builtin_popcount(mask); // next index in nums1
        if (i == n) continue;
        for (int j = 0; j < n; ++j) {
            if (mask & (1 << j)) continue; // j already used in nums2
            int newMask = mask | (1 << j);
            dp[newMask] = min(dp[newMask], dp[mask] + (nums1[i] ^ nums2[j]));
        }
    }
    return dp[(1 << n) - 1];
}

// ─────────────────────────────────────────────
// PROBLEM 11: Sum of Two Integers Without + or -  [Difficulty: Medium]
// Source: LeetCode 371
// ─────────────────────────────────────────────
// Approach: XOR for sum bits, AND<<1 for carry; repeat until no carry.
// Time: O(1)  Space: O(1)
int getSum(int a, int b) {
    while (b != 0) {
        unsigned carry = (unsigned)(a & b) << 1;
        a ^= b;
        b = (int)carry;
    }
    return a;
}

int main() {
    // Problem 1
    cout << singleNumber({4,1,2,1,2}) << "\n"; // 4

    // Problem 2
    auto p2 = singleNumberIII({1,2,1,3,2,5});
    cout << p2[0] << " " << p2[1] << "\n"; // 3 5 (order may vary)

    // Problem 3
    cout << missingNumber({3,0,1}) << "\n"; // 2

    // Problem 4
    auto p4 = findMissingAndDuplicate({1,2,2,4});
    cout << "missing=" << p4[0] << " dup=" << p4[1] << "\n"; // missing=3 dup=2

    // Problem 5
    cout << bitwiseComplement(5) << "\n"; // 2 (101 -> 010)

    // Problem 6
    auto p6 = flipAndInvertImage({{1,1,0},{1,0,1},{0,0,0}});
    for (auto& row : p6) { for (int v : row) cout << v << " "; cout << "\n"; }

    // Problem 7
    cout << boolalpha << isPowerOfTwo(16) << " " << isPowerOfTwo(3) << "\n"; // true false

    // Problem 8
    auto p8 = countBits(5);
    for (int v : p8) cout << v << " "; cout << "\n"; // 0 1 1 2 1 2

    // Problem 9
    auto p9 = xorQueries({1,3,4,8}, {{0,1},{1,2},{0,3},{3,3}});
    for (int v : p9) cout << v << " "; cout << "\n"; // 2 7 14 8

    // Problem 10
    cout << minimumXORSum({1,2},{2,3}) << "\n"; // 2 (1^2 + 2^3 = 3+1=4, 1^3+2^2=2+0=2)

    // Problem 11
    cout << getSum(1, 2) << "\n"; // 3

    return 0;
}
