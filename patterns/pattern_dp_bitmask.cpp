/*
 * PATTERN: Dynamic Programming — Bitmask
 *
 * CONCEPT:
 * When the problem has a small set S (|S| <= 20) and the state depends on
 * which subset of S has been "used/visited", encode the subset as a bitmask
 * integer. dp[mask] = optimal answer when the elements in `mask` have been
 * processed. Transition: dp[mask | (1<<i)] = f(dp[mask], item i).
 *
 * TIME:  O(2^n * n) typical
 * SPACE: O(2^n * n) or O(2^n)
 *
 * WHEN TO USE:
 * - "Assign n tasks to n workers optimally" (n <= 20)
 * - Traveling Salesman Problem
 * - "Cover all nodes" with minimum cost
 * - Sticker/hamiltonian-path problems over small sets
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Traveling Salesman Problem  [Difficulty: Hard]
// Source: Classic (Held-Karp)
// ─────────────────────────────────────────────
// Approach: dp[mask][i] = min cost to visit all cities in `mask` ending at i.
// Time: O(2^n * n²)  Space: O(2^n * n)
int tsp(const vector<vector<int>>& dist) {
    int n = (int)dist.size();
    vector<vector<int>> dp(1<<n, vector<int>(n, INT_MAX/2));
    dp[1][0] = 0; // start at city 0
    for (int mask = 1; mask < (1<<n); ++mask) {
        for (int u = 0; u < n; ++u) {
            if (!(mask & (1<<u))) continue;
            if (dp[mask][u] == INT_MAX/2) continue;
            for (int v = 0; v < n; ++v) {
                if (mask & (1<<v)) continue;
                int newMask = mask | (1<<v);
                dp[newMask][v] = min(dp[newMask][v], dp[mask][u] + dist[u][v]);
            }
        }
    }
    int full = (1<<n) - 1, result = INT_MAX;
    for (int i = 1; i < n; ++i)
        if (dp[full][i] != INT_MAX/2) result = min(result, dp[full][i] + dist[i][0]);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Minimum XOR Sum of Two Arrays  [Difficulty: Hard]
// Source: LeetCode 1879
// ─────────────────────────────────────────────
// Approach: dp[mask] = min XOR sum assigning first popcount(mask) of nums1 to nums2 elements in mask.
// Time: O(2^n * n)  Space: O(2^n)
int minimumXORSum(const vector<int>& nums1, const vector<int>& nums2) {
    int n = (int)nums1.size();
    vector<int> dp(1<<n, INT_MAX);
    dp[0] = 0;
    for (int mask = 0; mask < (1<<n); ++mask) {
        if (dp[mask] == INT_MAX) continue;
        int i = __builtin_popcount(mask); // next index in nums1
        if (i == n) continue;
        for (int j = 0; j < n; ++j) {
            if (mask & (1<<j)) continue;
            dp[mask | (1<<j)] = min(dp[mask | (1<<j)], dp[mask] + (nums1[i] ^ nums2[j]));
        }
    }
    return dp[(1<<n)-1];
}

// ─────────────────────────────────────────────
// PROBLEM 3: Number of Ways to Wear Different Hats  [Difficulty: Hard]
// Source: LeetCode 1434
// ─────────────────────────────────────────────
// Approach: dp[hat][personMask] = ways to assign hats 1..hat so that personMask are covered.
// Time: O(40 * 2^n * n)  Space: O(40 * 2^n)
int numberWays(const vector<vector<int>>& hats) {
    const int MOD = 1e9+7;
    int n = (int)hats.size();
    // hatToPeople[h] = list of people who can wear hat h
    vector<vector<int>> hatToPeople(41);
    for (int i = 0; i < n; ++i) for (int h : hats[i]) hatToPeople[h].push_back(i);
    int full = (1<<n)-1;
    vector<vector<long long>> dp(41, vector<long long>(full+1, 0));
    dp[0][0] = 1;
    for (int hat = 1; hat <= 40; ++hat) {
        for (int mask = 0; mask <= full; ++mask) {
            dp[hat][mask] = dp[hat-1][mask]; // skip this hat
            for (int person : hatToPeople[hat]) {
                if (mask & (1<<person)) {
                    dp[hat][mask] = (dp[hat][mask] + dp[hat-1][mask^(1<<person)]) % MOD;
                }
            }
        }
    }
    return (int)dp[40][full];
}

// ─────────────────────────────────────────────
// PROBLEM 4: Can I Win  [Difficulty: Medium]
// Source: LeetCode 464
// ─────────────────────────────────────────────
// Approach: Bitmask = set of numbers taken; memoize current total vs desiredTotal.
// Time: O(2^n * n)  Space: O(2^n)
unordered_map<int,bool> memo464;
bool canIWin(int maxChoosableInteger, int desiredTotal) {
    if (desiredTotal <= 0) return true;
    int total = maxChoosableInteger * (maxChoosableInteger+1) / 2;
    if (total < desiredTotal) return false;
    memo464.clear();
    function<bool(int,int)> dp = [&](int mask, int remaining) -> bool {
        if (memo464.count(mask)) return memo464[mask];
        for (int i = 1; i <= maxChoosableInteger; ++i) {
            if (mask & (1<<i)) continue;
            if (i >= remaining || !dp(mask|(1<<i), remaining-i))
                return memo464[mask] = true;
        }
        return memo464[mask] = false;
    };
    return dp(0, desiredTotal);
}

// ─────────────────────────────────────────────
// PROBLEM 5: Shortest Path Visiting All Nodes  [Difficulty: Hard]
// Source: LeetCode 847
// ─────────────────────────────────────────────
// Approach: BFS on (node, visitedMask); state = (current_node, bitmask).
// Time: O(2^n * n)  Space: O(2^n * n)
int shortestPathLength(const vector<vector<int>>& graph) {
    int n = (int)graph.size();
    int full = (1<<n)-1;
    queue<tuple<int,int,int>> q; // (node, mask, dist)
    vector<vector<bool>> visited(n, vector<bool>(1<<n, false));
    for (int i = 0; i < n; ++i) {
        q.push({i, 1<<i, 0});
        visited[i][1<<i] = true;
    }
    while (!q.empty()) {
        auto [node, mask, dist] = q.front(); q.pop();
        if (mask == full) return dist;
        for (int nb : graph[node]) {
            int newMask = mask | (1<<nb);
            if (!visited[nb][newMask]) {
                visited[nb][newMask] = true;
                q.push({nb, newMask, dist+1});
            }
        }
    }
    return 0;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Maximum Students Taking Exam  [Difficulty: Hard]
// Source: LeetCode 1349
// ─────────────────────────────────────────────
// Approach: Row-by-row bitmask DP; check validity of placing students.
// Time: O(m * 2^n * 2^n)  Space: O(2^n)
int maxStudents(const vector<vector<char>>& seats) {
    int m = (int)seats.size(), n = (int)seats[0].size();
    // Convert rows to bitmasks of valid seats
    vector<int> valid(m);
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < n; ++j)
            if (seats[i][j]=='.') valid[i] |= (1<<j);
    vector<int> dp(1<<n, 0);
    dp[0] = 0;
    for (int i = 0; i < m; ++i) {
        vector<int> ndp(1<<n, -1);
        for (int prev = 0; prev < (1<<n); ++prev) {
            if (dp[prev] < 0) continue;
            for (int cur = 0; cur < (1<<n); ++cur) {
                if (cur & ~valid[i]) continue;           // only valid seats
                if (cur & (cur>>1)) continue;            // no adjacent in same row
                if (cur & (prev<<1)) continue;           // no cheating from upper-right
                if (cur & (prev>>1)) continue;           // no cheating from upper-left
                ndp[cur] = max(ndp[cur], dp[prev] + __builtin_popcount(cur));
            }
        }
        dp = ndp;
    }
    return *max_element(dp.begin(), dp.end());
}

// ─────────────────────────────────────────────
// PROBLEM 7: Stickers to Spell Word  [Difficulty: Hard]
// Source: LeetCode 691
// ─────────────────────────────────────────────
// Approach: Bitmask over target chars; dp[mask] = min stickers to cover mask.
// Time: O(2^n * T * S)  Space: O(2^n)
int minStickers(const vector<string>& stickers, const string& target) {
    int n = (int)target.size();
    vector<int> dp(1<<n, INT_MAX);
    dp[0] = 0;
    for (int mask = 0; mask < (1<<n); ++mask) {
        if (dp[mask] == INT_MAX) continue;
        for (auto& s : stickers) {
            int cur = mask;
            for (char c : s) {
                // Use c to cover the first uncovered occurrence in target
                for (int i = 0; i < n; ++i) {
                    if (!(cur & (1<<i)) && target[i] == c) { cur |= (1<<i); break; }
                }
            }
            dp[cur] = min(dp[cur], dp[mask] + 1);
        }
    }
    return dp[(1<<n)-1] == INT_MAX ? -1 : dp[(1<<n)-1];
}

// ─────────────────────────────────────────────
// PROBLEM 8: Count Ways to Assign Couriers  [Difficulty: Medium]
// Source: Classic — count permutations with bitmask
// ─────────────────────────────────────────────
// dp[mask] = number of ways to assign items in mask to first popcount(mask) couriers.
// Time: O(2^n * n)  Space: O(2^n)
long long countAssignments(const vector<vector<int>>& canDeliver) {
    int n = (int)canDeliver.size();
    vector<long long> dp(1<<n, 0); dp[0] = 1;
    for (int mask = 0; mask < (1<<n); ++mask) {
        if (!dp[mask]) continue;
        int courier = __builtin_popcount(mask);
        if (courier == n) continue;
        for (int item = 0; item < n; ++item) {
            if ((mask & (1<<item)) || !canDeliver[courier][item]) continue;
            dp[mask|(1<<item)] += dp[mask];
        }
    }
    return dp[(1<<n)-1];
}

// ─────────────────────────────────────────────
// PROBLEM 9: Maximize Score After N Operations  [Difficulty: Hard]
// Source: LeetCode 1799
// ─────────────────────────────────────────────
// Approach: dp[mask] = max score using the pairs in mask; pick 2 at a time.
// Time: O(2^(2n) * n²)  Space: O(2^(2n))
int maxScore(const vector<int>& nums) {
    int n = (int)nums.size(); // n = 2*numOps
    int total = 1<<n;
    vector<int> dp(total, 0);
    // Precompute GCDs
    vector<vector<int>> g(n, vector<int>(n, 0));
    for (int i = 0; i < n; ++i) for (int j = i+1; j < n; ++j)
        g[i][j] = __gcd(nums[i], nums[j]);
    for (int mask = 0; mask < total; ++mask) {
        int bits = __builtin_popcount(mask);
        if (bits % 2 != 0) continue;
        int op = bits / 2 + 1; // next operation number
        for (int i = 0; i < n; ++i) {
            if (mask & (1<<i)) continue;
            for (int j = i+1; j < n; ++j) {
                if (mask & (1<<j)) continue;
                int newMask = mask | (1<<i) | (1<<j);
                dp[newMask] = max(dp[newMask], dp[mask] + op * g[i][j]);
            }
        }
    }
    return dp[total-1];
}

int main() {
    // Problem 1
    cout << tsp({{0,10,15,20},{10,0,35,25},{15,35,0,30},{20,25,30,0}}) << "\n"; // 80

    // Problem 2
    cout << minimumXORSum({1,2},{2,3}) << "\n"; // 2

    // Problem 3
    cout << numberWays({{3,4},{4,5},{5}}) << "\n"; // 1

    // Problem 4
    cout << boolalpha << canIWin(10, 11) << "\n"; // false
    cout << canIWin(10, 40) << "\n"; // false

    // Problem 5
    cout << shortestPathLength({{1,2,3},{0},{0},{0}}) << "\n"; // 4

    // Problem 6
    cout << maxStudents({{'#','.','#','#','.','#'},
                         {'.','#','#','#','#','.'},
                         {'#','.','#','#','.','#'}}) << "\n"; // 4

    // Problem 7
    cout << minStickers({"with","example","science"}, "thehat") << "\n"; // 3

    // Problem 8
    cout << countAssignments({{1,1,0},{0,1,1},{1,0,1}}) << "\n"; // 2

    // Problem 9
    cout << maxScore({1,2,3,4,5,6}) << "\n"; // 14

    return 0;
}
