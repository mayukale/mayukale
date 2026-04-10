/*
 * PATTERN: Greedy
 *
 * CONCEPT:
 * Make the locally optimal choice at each step with the hope of reaching a
 * globally optimal solution. Works when the problem has the "greedy choice
 * property" (a global optimum can be reached by local optima) and "optimal
 * substructure". Often requires sorting by a key first.
 *
 * TIME:  O(n log n) for most (dominated by sort)
 * SPACE: O(1) or O(n)
 *
 * WHEN TO USE:
 * - Scheduling / interval problems (sort by end time)
 * - "Minimum number of X to cover Y"
 * - Jump game, gas station, task assignment
 * - Huffman coding, fractional knapsack
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Jump Game  [Difficulty: Medium]
// Source: LeetCode 55
// ─────────────────────────────────────────────
// Approach: Track max reachable index; if i > maxReach at any point, return false.
// Time: O(n)  Space: O(1)
bool canJump(const vector<int>& nums) {
    int maxReach = 0;
    for (int i = 0; i < (int)nums.size(); ++i) {
        if (i > maxReach) return false;
        maxReach = max(maxReach, i + nums[i]);
    }
    return true;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Jump Game II (min jumps)  [Difficulty: Medium]
// Source: LeetCode 45
// ─────────────────────────────────────────────
// Approach: Track current window end and furthest reachable; jump when window expires.
// Time: O(n)  Space: O(1)
int jump(const vector<int>& nums) {
    int jumps = 0, curEnd = 0, farthest = 0;
    for (int i = 0; i < (int)nums.size() - 1; ++i) {
        farthest = max(farthest, i + nums[i]);
        if (i == curEnd) { ++jumps; curEnd = farthest; }
    }
    return jumps;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Gas Station  [Difficulty: Medium]
// Source: LeetCode 134
// ─────────────────────────────────────────────
// Approach: If total gas >= total cost, solution exists; start is where cumulative sum dips lowest.
// Time: O(n)  Space: O(1)
int canCompleteCircuit(const vector<int>& gas, const vector<int>& cost) {
    int total = 0, tank = 0, start = 0;
    for (int i = 0; i < (int)gas.size(); ++i) {
        int diff = gas[i] - cost[i];
        total += diff; tank += diff;
        if (tank < 0) { start = i + 1; tank = 0; }
    }
    return total >= 0 ? start : -1;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Assign Cookies  [Difficulty: Easy]
// Source: LeetCode 455
// ─────────────────────────────────────────────
// Approach: Sort both; greedily assign smallest sufficient cookie to each child.
// Time: O(n log n)  Space: O(1)
int findContentChildren(vector<int> g, vector<int> s) {
    sort(g.begin(), g.end()); sort(s.begin(), s.end());
    int child = 0, cookie = 0;
    while (child < (int)g.size() && cookie < (int)s.size()) {
        if (s[cookie] >= g[child]) ++child;
        ++cookie;
    }
    return child;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Partition Labels  [Difficulty: Medium]
// Source: LeetCode 763
// ─────────────────────────────────────────────
// Approach: Record last occurrence of each char; greedily extend current partition.
// Time: O(n)  Space: O(26)
vector<int> partitionLabels(const string& s) {
    vector<int> last(26, 0);
    for (int i = 0; i < (int)s.size(); ++i) last[s[i]-'a'] = i;
    vector<int> result;
    int start = 0, end = 0;
    for (int i = 0; i < (int)s.size(); ++i) {
        end = max(end, last[s[i]-'a']);
        if (i == end) { result.push_back(end - start + 1); start = i + 1; }
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Candy Distribution  [Difficulty: Hard]
// Source: LeetCode 135
// ─────────────────────────────────────────────
// Approach: Two-pass greedy: left→right for ascending runs, right→left for descending.
// Time: O(n)  Space: O(n)
int candy(const vector<int>& ratings) {
    int n = (int)ratings.size();
    vector<int> candies(n, 1);
    for (int i = 1; i < n; ++i)
        if (ratings[i] > ratings[i-1]) candies[i] = candies[i-1] + 1;
    for (int i = n-2; i >= 0; --i)
        if (ratings[i] > ratings[i+1]) candies[i] = max(candies[i], candies[i+1]+1);
    return accumulate(candies.begin(), candies.end(), 0);
}

// ─────────────────────────────────────────────
// PROBLEM 7: Minimum Number of Platforms / Meeting Rooms  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Sort arrivals and departures separately; sweep with two pointers.
// Time: O(n log n)  Space: O(n)
int minPlatforms(vector<int> arr, vector<int> dep) {
    sort(arr.begin(), arr.end()); sort(dep.begin(), dep.end());
    int n = (int)arr.size(), platforms = 1, maxPlat = 1, j = 0;
    for (int i = 1; i < n; ++i) {
        if (arr[i] <= dep[j]) { ++platforms; ++maxPlat; }
        else { --platforms; ++j; }
        maxPlat = max(maxPlat, platforms);
    }
    return maxPlat;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Lemonade Change  [Difficulty: Easy]
// Source: LeetCode 860
// ─────────────────────────────────────────────
// Approach: Greedily use $10 bill before $5 bill when making change for $20.
// Time: O(n)  Space: O(1)
bool lemonadeChange(const vector<int>& bills) {
    int five = 0, ten = 0;
    for (int b : bills) {
        if (b == 5) ++five;
        else if (b == 10) { if (!five) return false; --five; ++ten; }
        else { // b == 20
            if (ten && five) { --ten; --five; }
            else if (five >= 3) five -= 3;
            else return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Minimum Operations to Reduce X to Zero  [Difficulty: Medium]
// Source: LeetCode 1658
// ─────────────────────────────────────────────
// Approach: Equivalent to longest subarray with sum = total - x (sliding window).
// Time: O(n)  Space: O(1)
int minOperations(const vector<int>& nums, int x) {
    int target = accumulate(nums.begin(), nums.end(), 0) - x;
    if (target < 0) return -1;
    if (target == 0) return (int)nums.size();
    int left = 0, sum = 0, maxLen = -1;
    for (int right = 0; right < (int)nums.size(); ++right) {
        sum += nums[right];
        while (sum > target) sum -= nums[left++];
        if (sum == target) maxLen = max(maxLen, right - left + 1);
    }
    return maxLen == -1 ? -1 : (int)nums.size() - maxLen;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Activity Selection  [Difficulty: Medium]
// Source: Classic CLRS
// ─────────────────────────────────────────────
// Approach: Sort by finish time; greedily pick each activity that starts after last picked.
// Time: O(n log n)  Space: O(1)
int activitySelection(vector<pair<int,int>> activities) {
    sort(activities.begin(), activities.end(),
         [](const auto& a, const auto& b){ return a.second < b.second; });
    int count = 1, lastEnd = activities[0].second;
    for (int i = 1; i < (int)activities.size(); ++i) {
        if (activities[i].first >= lastEnd) {
            ++count; lastEnd = activities[i].second;
        }
    }
    return count;
}

// ─────────────────────────────────────────────
// PROBLEM 11: Minimum Cost to Hire K Workers  [Difficulty: Hard]
// Source: LeetCode 857
// ─────────────────────────────────────────────
// Approach: Sort by wage/quality ratio; slide window of size k using max-heap on quality.
// Time: O(n log n + n log k)  Space: O(n)
double mincostToHireWorkers(const vector<int>& quality, const vector<int>& wage, int k) {
    int n = (int)quality.size();
    vector<pair<double,int>> workers(n);
    for (int i = 0; i < n; ++i) workers[i] = {(double)wage[i]/quality[i], quality[i]};
    sort(workers.begin(), workers.end());
    priority_queue<int> pq; // max-heap of quality
    int qualSum = 0;
    double ans = DBL_MAX;
    for (auto& [ratio, q] : workers) {
        pq.push(q); qualSum += q;
        if ((int)pq.size() > k) { qualSum -= pq.top(); pq.pop(); }
        if ((int)pq.size() == k) ans = min(ans, ratio * qualSum);
    }
    return ans;
}

int main() {
    // Problem 1
    cout << boolalpha << canJump({2,3,1,1,4}) << "\n"; // true
    cout << canJump({3,2,1,0,4}) << "\n"; // false

    // Problem 2
    cout << jump({2,3,1,1,4}) << "\n"; // 2

    // Problem 3
    cout << canCompleteCircuit({1,2,3,4,5}, {3,4,5,1,2}) << "\n"; // 3

    // Problem 4
    cout << findContentChildren({1,2,3}, {1,1}) << "\n"; // 1

    // Problem 5
    auto p5 = partitionLabels("ababcbacadefegdehijhklij");
    for (int v : p5) cout << v << " "; cout << "\n"; // 9 7 8

    // Problem 6
    cout << candy({1,0,2}) << "\n"; // 5

    // Problem 7
    cout << minPlatforms({900,940,950,1100,1500,1800}, {910,1200,1120,1130,1900,2000}) << "\n"; // 3

    // Problem 8
    cout << lemonadeChange({5,5,10,20,5,5,10,10,20,20}) << "\n"; // false

    // Problem 9
    cout << minOperations({1,1,4,2,3}, 5) << "\n"; // 2

    // Problem 10
    cout << activitySelection({{1,2},{3,4},{0,6},{5,7},{8,9},{5,9}}) << "\n"; // 4

    // Problem 11
    cout << fixed << setprecision(5)
         << mincostToHireWorkers({10,20,5},{70,50,30}, 2) << "\n"; // 105.0

    return 0;
}
