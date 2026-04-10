/*
 * PATTERN: Top K Elements
 *
 * CONCEPT:
 * Maintain a heap of size k while scanning an array. A min-heap of size k
 * keeps the k largest elements (evict the smallest when overfull). A max-heap
 * of size k keeps the k smallest. For "k-th" queries, the heap top is the answer.
 * For frequency-based "top k", build a frequency map first, then heap on freq.
 *
 * TIME:  O(n log k)
 * SPACE: O(k)
 *
 * WHEN TO USE:
 * - "Find top/bottom K elements by value or frequency"
 * - "K-th largest/smallest element"
 * - "K closest points", "K most frequent words"
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Kth Largest Element in an Array  [Difficulty: Medium]
// Source: LeetCode 215
// ─────────────────────────────────────────────
// Approach: Min-heap of size k; top = k-th largest.
// Time: O(n log k)  Space: O(k)
int findKthLargest(const vector<int>& nums, int k) {
    priority_queue<int, vector<int>, greater<int>> minHeap;
    for (int n : nums) {
        minHeap.push(n);
        if ((int)minHeap.size() > k) minHeap.pop();
    }
    return minHeap.top();
}

// ─────────────────────────────────────────────
// PROBLEM 2: Top K Frequent Elements  [Difficulty: Medium]
// Source: LeetCode 347
// ─────────────────────────────────────────────
// Approach: Frequency map + min-heap of size k on (freq, val).
// Time: O(n log k)  Space: O(n)
vector<int> topKFrequent(const vector<int>& nums, int k) {
    unordered_map<int, int> freq;
    for (int n : nums) ++freq[n];
    priority_queue<pair<int,int>, vector<pair<int,int>>, greater<>> minHeap;
    for (auto& [val, cnt] : freq) {
        minHeap.push({cnt, val});
        if ((int)minHeap.size() > k) minHeap.pop();
    }
    vector<int> result;
    while (!minHeap.empty()) { result.push_back(minHeap.top().second); minHeap.pop(); }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Top K Frequent Words  [Difficulty: Medium]
// Source: LeetCode 692
// ─────────────────────────────────────────────
// Approach: Freq map; max-heap ordered by (freq desc, lex asc).
// Time: O(n log k)  Space: O(n)
vector<string> topKFrequentWords(const vector<string>& words, int k) {
    unordered_map<string, int> freq;
    for (auto& w : words) ++freq[w];
    // Custom comparator: higher freq wins; ties break by lex order (smaller is "greater" in heap)
    auto cmp = [](const pair<int,string>& a, const pair<int,string>& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    };
    priority_queue<pair<int,string>, vector<pair<int,string>>, decltype(cmp)> minHeap(cmp);
    for (auto& [word, cnt] : freq) {
        minHeap.push({cnt, word});
        if ((int)minHeap.size() > k) minHeap.pop();
    }
    vector<string> result;
    while (!minHeap.empty()) { result.push_back(minHeap.top().second); minHeap.pop(); }
    reverse(result.begin(), result.end());
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: K Closest Points to Origin  [Difficulty: Medium]
// Source: LeetCode 973
// ─────────────────────────────────────────────
// Approach: Max-heap of (dist², index) of size k.
// Time: O(n log k)  Space: O(k)
vector<vector<int>> kClosest(const vector<vector<int>>& points, int k) {
    auto dist2 = [](const vector<int>& p) { return p[0]*p[0] + p[1]*p[1]; };
    priority_queue<pair<int,int>> maxHeap;
    for (int i = 0; i < (int)points.size(); ++i) {
        maxHeap.push({dist2(points[i]), i});
        if ((int)maxHeap.size() > k) maxHeap.pop();
    }
    vector<vector<int>> result;
    while (!maxHeap.empty()) { result.push_back(points[maxHeap.top().second]); maxHeap.pop(); }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Sort Characters By Frequency  [Difficulty: Medium]
// Source: LeetCode 451
// ─────────────────────────────────────────────
// Approach: Frequency map, max-heap, rebuild string greedily.
// Time: O(n log n)  Space: O(n)
string frequencySort(const string& s) {
    unordered_map<char, int> freq;
    for (char c : s) ++freq[c];
    priority_queue<pair<int,char>> maxHeap;
    for (auto& [c, cnt] : freq) maxHeap.push({cnt, c});
    string result;
    while (!maxHeap.empty()) {
        auto [cnt, c] = maxHeap.top(); maxHeap.pop();
        result.append(cnt, c);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Reorganize String  [Difficulty: Medium]
// Source: LeetCode 767
// ─────────────────────────────────────────────
// Approach: Max-heap by frequency; greedily place most frequent, then second.
// Time: O(n log 26)  Space: O(26)
string reorganizeString(const string& s) {
    vector<int> freq(26, 0);
    for (char c : s) ++freq[c - 'a'];
    priority_queue<pair<int,char>> pq;
    for (int i = 0; i < 26; ++i) if (freq[i]) pq.push({freq[i], 'a'+i});
    string result;
    while (pq.size() >= 2) {
        auto [c1, ch1] = pq.top(); pq.pop();
        auto [c2, ch2] = pq.top(); pq.pop();
        result += ch1; result += ch2;
        if (c1 - 1) pq.push({c1 - 1, ch1});
        if (c2 - 1) pq.push({c2 - 1, ch2});
    }
    if (!pq.empty()) {
        if (pq.top().first > 1) return "";
        result += pq.top().second;
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: K-th Smallest Element in a Sorted Matrix  [Difficulty: Medium]
// Source: LeetCode 378
// ─────────────────────────────────────────────
// Approach: Min-heap of (val, row, col); pop k times advancing along row.
// Time: O(k log n)  Space: O(n)
int kthSmallestMatrix(const vector<vector<int>>& matrix, int k) {
    int n = (int)matrix.size();
    using T = tuple<int,int,int>;
    priority_queue<T, vector<T>, greater<T>> minHeap;
    for (int r = 0; r < n; ++r) minHeap.push({matrix[r][0], r, 0});
    int result = 0;
    for (int i = 0; i < k; ++i) {
        auto [val, r, c] = minHeap.top(); minHeap.pop();
        result = val;
        if (c + 1 < n) minHeap.push({matrix[r][c+1], r, c+1});
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Find K Pairs with Smallest Sums  [Difficulty: Medium]
// Source: LeetCode 373
// ─────────────────────────────────────────────
// Approach: Min-heap seeded with (nums1[i], nums2[0]) for each i; advance j.
// Time: O(k log k)  Space: O(k)
vector<vector<int>> kSmallestPairs(const vector<int>& nums1,
                                   const vector<int>& nums2, int k) {
    using T = tuple<int,int,int>; // (sum, i, j)
    priority_queue<T, vector<T>, greater<T>> pq;
    for (int i = 0; i < min(k, (int)nums1.size()); ++i)
        pq.push({nums1[i] + nums2[0], i, 0});
    vector<vector<int>> result;
    while (!pq.empty() && (int)result.size() < k) {
        auto [s, i, j] = pq.top(); pq.pop();
        result.push_back({nums1[i], nums2[j]});
        if (j + 1 < (int)nums2.size()) pq.push({nums1[i] + nums2[j+1], i, j+1});
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Minimum Cost to Connect Sticks (Huffman)  [Difficulty: Medium]
// Source: LeetCode 1167
// ─────────────────────────────────────────────
// Approach: Min-heap; always merge two smallest, accumulate cost.
// Time: O(n log n)  Space: O(n)
int connectSticks(vector<int> sticks) {
    priority_queue<int, vector<int>, greater<int>> pq(sticks.begin(), sticks.end());
    int cost = 0;
    while (pq.size() > 1) {
        int a = pq.top(); pq.pop();
        int b = pq.top(); pq.pop();
        cost += a + b;
        pq.push(a + b);
    }
    return cost;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Ugly Number II  [Difficulty: Medium]
// Source: LeetCode 264
// ─────────────────────────────────────────────
// Approach: Min-heap; generate candidates by multiplying by 2,3,5; skip duplicates.
// Time: O(n log n)  Space: O(n)
int nthUglyNumber(int n) {
    priority_queue<long long, vector<long long>, greater<long long>> pq;
    pq.push(1);
    unordered_set<long long> seen = {1};
    long long curr = 1;
    const vector<int> factors = {2, 3, 5};
    for (int i = 0; i < n; ++i) {
        curr = pq.top(); pq.pop();
        for (int f : factors) {
            long long next = curr * f;
            if (!seen.count(next)) { seen.insert(next); pq.push(next); }
        }
    }
    return (int)curr;
}

int main() {
    // Problem 1
    cout << findKthLargest({3,2,1,5,6,4}, 2) << "\n"; // 5

    // Problem 2
    auto p2 = topKFrequent({1,1,1,2,2,3}, 2);
    for (int v : p2) cout << v << " "; cout << "\n"; // 1 2

    // Problem 3
    auto p3 = topKFrequentWords({"i","love","leetcode","i","love","coding"}, 2);
    for (auto& w : p3) cout << w << " "; cout << "\n"; // i love

    // Problem 4
    auto p4 = kClosest({{1,3},{-2,2}}, 1);
    cout << "[" << p4[0][0] << "," << p4[0][1] << "]\n"; // [-2,2]

    // Problem 5
    cout << frequencySort("tree") << "\n"; // eert or eetr

    // Problem 6
    cout << reorganizeString("aab") << "\n"; // aba

    // Problem 7
    cout << kthSmallestMatrix({{1,5,9},{10,11,13},{12,13,15}}, 8) << "\n"; // 13

    // Problem 8
    auto p8 = kSmallestPairs({1,7,11}, {2,4,6}, 3);
    for (auto& v : p8) cout << "[" << v[0] << "," << v[1] << "] "; cout << "\n"; // [1,2][1,4][1,6]

    // Problem 9
    cout << connectSticks({2,4,3}) << "\n"; // 14

    // Problem 10
    cout << nthUglyNumber(10) << "\n"; // 12

    return 0;
}
