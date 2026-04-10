/*
 * PATTERN: Dynamic Programming — Digit DP
 *
 * CONCEPT:
 * Count integers in range [L, R] that satisfy a digit-level constraint
 * (e.g., no two adjacent same digits, digit sum = k, monotone digits).
 * Use count(R) - count(L-1) by defining count(N) = # valid integers in [1, N].
 * State: (position, tight, leading_zero, accumulated_sum/last_digit).
 * "tight" = whether we're still constrained by N's upper bound at this digit.
 *
 * TIME:  O(digits * states) with memoization
 * SPACE: O(digits * states)
 *
 * WHEN TO USE:
 * - "Count integers in [A, B] satisfying digit property"
 * - Digit sum, digit product, distinct digits
 * - No adjacent equal digits, monotone increasing/decreasing
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// Helper: convert N to digit vector (most-significant first)
vector<int> getDigits(long long N) {
    string s = to_string(N);
    vector<int> d(s.begin(), s.end());
    for (int& x : d) x -= '0';
    return d;
}

// ─────────────────────────────────────────────
// PROBLEM 1: Count Numbers with Unique Digits  [Difficulty: Medium]
// Source: LeetCode 357
// ─────────────────────────────────────────────
// Approach: dp[i] = count of i-digit numbers with all unique digits.
//           Use combinatorics: 9 * 9 * 8 * ... choices per position.
// Time: O(n)  Space: O(1)
int countNumbersWithUniqueDigits(int n) {
    if (n == 0) return 1;
    int result = 10, choices = 9;
    for (int i = 2; i <= n && i <= 10; ++i) {
        choices *= (10 - i + 1);
        result += 9 * choices / (9);
    }
    // Correct formula
    result = 10;
    int factor = 9;
    for (int i = 2; i <= n; ++i) {
        factor *= (11 - i);
        result += factor;
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Non-decreasing Digits  [Difficulty: Medium]
// Source: LeetCode 738
// ─────────────────────────────────────────────
// Approach: Greedy from right — find where digit decreases, decrement, fill 9s.
// Time: O(d) where d = # digits  Space: O(d)
int monotoneIncreasingDigits(int n) {
    string s = to_string(n);
    int mark = (int)s.size();
    for (int i = (int)s.size()-1; i > 0; --i) {
        if (s[i] < s[i-1]) { --s[i-1]; mark = i; }
    }
    for (int i = mark; i < (int)s.size(); ++i) s[i] = '9';
    return stoi(s);
}

// ─────────────────────────────────────────────
// PROBLEM 3: Count Digit One  [Difficulty: Hard]
// Source: LeetCode 233
// ─────────────────────────────────────────────
// Approach: For each digit position, count how many 1s appear there in [1..n].
// Time: O(log n)  Space: O(1)
int countDigitOne(int n) {
    long long count = 0, factor = 1;
    while (factor <= n) {
        long long cur   = (n / factor) % 10;
        long long high  = n / (factor * 10);
        long long low   = n % factor;
        if (cur == 0)       count += high * factor;
        else if (cur == 1)  count += high * factor + low + 1;
        else                count += (high + 1) * factor;
        factor *= 10;
    }
    return (int)count;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Numbers At Most N Given Digit Set  [Difficulty: Hard]
// Source: LeetCode 902
// ─────────────────────────────────────────────
// Approach: Digit DP with tight constraint; count shorter then same-length.
// Time: O(d * |D|)  Space: O(d)
int atMostNGivenDigitSet(const vector<string>& digits, int n) {
    string ns = to_string(n);
    int d = (int)ns.size(), k = (int)digits.size();
    int result = 0;
    // Numbers with fewer digits: k^1 + k^2 + ... + k^(d-1)
    int power = 1;
    for (int i = 1; i < d; ++i) { power *= k; result += power; }
    // Numbers with exactly d digits, <= n
    for (int i = 0; i < d; ++i) {
        // Count how many in digits[] < ns[i]
        int smaller = 0;
        for (auto& s : digits) if (s[0] < ns[i]) ++smaller;
        // For each smaller digit, k^(d-1-i) completions
        int completions = 1;
        for (int j = 0; j < d-1-i; ++j) completions *= k;
        result += smaller * completions;
        // Check if ns[i] itself is in digits
        bool found = false;
        for (auto& s : digits) if (s[0] == ns[i]) { found = true; break; }
        if (!found) break;
        if (i == d-1) ++result; // n itself
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Digit DP: Count integers in [1..N] with digit sum S  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// State: (pos, tight, leadingZero, sumSoFar) → memoized recursion.
// Time: O(digits * S)  Space: O(digits * S)
long long countWithDigitSumHelper(int pos, bool tight, bool leadingZero,
                                   int sum, int target,
                                   const vector<int>& digits,
                                   vector<vector<vector<vector<long long>>>>& memo) {
    if (sum > target) return 0;
    if (pos == (int)digits.size()) return (!leadingZero && sum == target) ? 1 : 0;
    auto& ref = memo[pos][tight?1:0][leadingZero?1:0][sum];
    if (ref != -1) return ref;
    int limit = tight ? digits[pos] : 9;
    long long result = 0;
    for (int d = 0; d <= limit; ++d) {
        bool newTight = tight && (d == limit);
        bool newLeading = leadingZero && (d == 0);
        result += countWithDigitSumHelper(pos+1, newTight, newLeading,
                                          newLeading ? 0 : sum + d, target, digits, memo);
    }
    return ref = result;
}
long long countWithDigitSum(long long N, int target) {
    auto digs = getDigits(N);
    int n = (int)digs.size();
    // memo[pos][tight][leadingZero][sum]
    vector<vector<vector<vector<long long>>>> memo(
        n, vector<vector<vector<long long>>>(
            2, vector<vector<long long>>(
                2, vector<long long>(target+1, -1))));
    return countWithDigitSumHelper(0, true, true, 0, target, digs, memo);
}

// ─────────────────────────────────────────────
// PROBLEM 6: Count Stepping Numbers  [Difficulty: Hard]
// Source: LeetCode 1215
// ─────────────────────────────────────────────
// A stepping number: adjacent digits differ by exactly 1.
// Approach: Digit DP with last digit as state.
// Time: O(d * 10)  Space: O(d * 10)
long long countSteppingHelper(int pos, int lastDigit, bool tight, bool leadingZero,
                               const vector<int>& digits,
                               vector<vector<vector<long long>>>& memo) {
    if (pos == (int)digits.size()) return leadingZero ? 0 : 1;
    auto& ref = memo[pos][lastDigit+1][tight?1:0]; // +1 for leadingZero marker
    if (ref != -1 && !leadingZero) return ref;
    int limit = tight ? digits[pos] : 9;
    long long result = 0;
    for (int d = 0; d <= limit; ++d) {
        if (!leadingZero && abs(d - lastDigit) != 1) continue;
        bool newLeading = leadingZero && (d == 0);
        result += countSteppingHelper(pos+1, newLeading ? -1 : d,
                                       tight && (d==limit), newLeading, digits, memo);
    }
    if (!leadingZero) ref = result;
    return result;
}
long long countStepping(long long N) {
    auto digs = getDigits(N);
    int n = (int)digs.size();
    vector<vector<vector<long long>>> memo(n, vector<vector<long long>>(12, vector<long long>(2, -1)));
    return countSteppingHelper(0, -1, true, true, digs, memo);
}

// ─────────────────────────────────────────────
// PROBLEM 7: Rotated Digits  [Difficulty: Medium]
// Source: LeetCode 788
// ─────────────────────────────────────────────
// Approach: Digit DP; digits {2,5,6,9} rotate differently; must contain at least one.
// Time: O(d * 2)  Space: O(d * 2)
// (simple enough for direct simulation too)
int rotatedDigits(int n) {
    // Valid: all digits in {0,1,2,5,6,8,9}; good if has at least one of {2,5,6,9}
    const unordered_set<int> bad = {3,4,7};
    const unordered_set<int> diff = {2,5,6,9};
    int count = 0;
    for (int i = 1; i <= n; ++i) {
        int x = i; bool valid = true, good = false;
        while (x > 0) {
            int d = x % 10;
            if (bad.count(d)) { valid = false; break; }
            if (diff.count(d)) good = true;
            x /= 10;
        }
        if (valid && good) ++count;
    }
    return count;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Sum of All Subset XOR Totals  [Difficulty: Easy]
// Source: LeetCode 1863 (digit-like bit counting)
// ─────────────────────────────────────────────
// Approach: XOR of all subsets = OR of all elements * 2^(n-1).
// Time: O(n)  Space: O(1)
int subsetXORSum(const vector<int>& nums) {
    int orAll = 0;
    for (int n : nums) orAll |= n;
    return orAll << ((int)nums.size() - 1);
}

// ─────────────────────────────────────────────
// PROBLEM 9: Numbers With Same Consecutive Differences  [Difficulty: Medium]
// Source: LeetCode 967
// ─────────────────────────────────────────────
// Approach: BFS/DFS building digits one at a time; append d such that |d-prev|==k.
// Time: O(n * 10)  Space: O(n * 10)
vector<int> numsSameConsecDiff(int n, int k) {
    vector<int> cur = {1,2,3,4,5,6,7,8,9};
    for (int i = 1; i < n; ++i) {
        vector<int> next;
        for (int num : cur) {
            int lastDigit = num % 10;
            if (lastDigit + k <= 9) next.push_back(num * 10 + lastDigit + k);
            if (k != 0 && lastDigit - k >= 0) next.push_back(num * 10 + lastDigit - k);
        }
        cur = next;
    }
    return cur;
}

// ─────────────────────────────────────────────
// PROBLEM 10: K-th Digit in 1...N  [Difficulty: Hard]
// Source: LeetCode 400
// ─────────────────────────────────────────────
// Approach: Determine how many digits each "length class" contributes.
// Time: O(log n)  Space: O(1)
int findNthDigit(int n) {
    long long digits = 1, count = 9, start = 1;
    while (n > digits * count) {
        n -= (int)(digits * count);
        ++digits; count *= 10; start *= 10;
    }
    long long num = start + (n-1) / digits;
    int digitIndex = (n-1) % digits;
    string s = to_string(num);
    return s[digitIndex] - '0';
}

int main() {
    // Problem 1
    cout << countNumbersWithUniqueDigits(2) << "\n"; // 91

    // Problem 2
    cout << monotoneIncreasingDigits(332) << "\n"; // 299

    // Problem 3
    cout << countDigitOne(13) << "\n"; // 6

    // Problem 4
    cout << atMostNGivenDigitSet({"1","3","5","7"}, 100) << "\n"; // 20

    // Problem 5
    cout << countWithDigitSum(100, 10) << "\n"; // numbers in [1..100] with digit sum 10

    // Problem 6
    cout << countStepping(100) << "\n"; // stepping numbers in [1..100]

    // Problem 7
    cout << rotatedDigits(10) << "\n"; // 4 (2,5,6,9)

    // Problem 8
    cout << subsetXORSum({1,3}) << "\n"; // 6

    // Problem 9
    auto p9 = numsSameConsecDiff(3, 7);
    for (int v : p9) cout << v << " "; cout << "\n"; // 181 292 707 818 929

    // Problem 10
    cout << findNthDigit(11) << "\n"; // 0

    return 0;
}
