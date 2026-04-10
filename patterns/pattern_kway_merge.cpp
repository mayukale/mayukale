/*
 * PATTERN: K-way Merge
 *
 * CONCEPT:
 * When merging k sorted lists/arrays simultaneously, use a min-heap seeded
 * with the first element of each list. Repeatedly pop the global minimum,
 * output it, and push the next element from the same list. This gives a
 * unified sorted stream in O(n log k) time.
 *
 * TIME:  O(n log k) — n total elements, k lists
 * SPACE: O(k)
 *
 * WHEN TO USE:
 * - Merge k sorted lists/arrays
 * - K-th smallest across k sorted structures
 * - External sort, distributed merge
 * - "Smallest range covering elements from k lists"
 */

#include <bits/stdc++.h>
using namespace std;

struct ListNode {
    int val; ListNode* next;
    explicit ListNode(int v, ListNode* n = nullptr) : val(v), next(n) {}
};
ListNode* buildList(initializer_list<int> vals) {
    ListNode dummy(0); ListNode* cur = &dummy;
    for (int v : vals) { cur->next = new ListNode(v); cur = cur->next; }
    return dummy.next;
}
void printList(ListNode* h) { for (; h; h = h->next) cout << h->val << (h->next?"->":" "); cout << "\n"; }

// ─────────────────────────────────────────────
// PROBLEM 1: Merge K Sorted Lists  [Difficulty: Hard]
// Source: LeetCode 23
// ─────────────────────────────────────────────
// Approach: Min-heap of (val, list_index, node_ptr); extract min, advance list.
// Time: O(n log k)  Space: O(k)
ListNode* mergeKLists(vector<ListNode*> lists) {
    using T = pair<int, ListNode*>;
    priority_queue<T, vector<T>, greater<T>> pq;
    for (auto* l : lists) if (l) pq.push({l->val, l});
    ListNode dummy(0); ListNode* tail = &dummy;
    while (!pq.empty()) {
        auto [val, node] = pq.top(); pq.pop();
        tail->next = node; tail = tail->next;
        if (node->next) pq.push({node->next->val, node->next});
    }
    return dummy.next;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Kth Smallest Element in K Sorted Arrays  [Difficulty: Medium]
// Source: Classic / LeetCode 373 variant
// ─────────────────────────────────────────────
// Approach: Min-heap of (val, row, col); pop k times.
// Time: O(k log k + result*log k)  Space: O(k)
int kthSmallestInKArrays(const vector<vector<int>>& arrays, int k) {
    using T = tuple<int,int,int>; // val, row, col
    priority_queue<T, vector<T>, greater<T>> pq;
    for (int i = 0; i < (int)arrays.size(); ++i)
        if (!arrays[i].empty()) pq.push({arrays[i][0], i, 0});
    int result = 0;
    for (int i = 0; i < k; ++i) {
        auto [val, r, c] = pq.top(); pq.pop();
        result = val;
        if (c + 1 < (int)arrays[r].size()) pq.push({arrays[r][c+1], r, c+1});
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Smallest Range Covering Elements from K Lists  [Difficulty: Hard]
// Source: LeetCode 632
// ─────────────────────────────────────────────
// Approach: Min-heap + track current max; slide window of one element per list.
// Time: O(n log k)  Space: O(k)
vector<int> smallestRange(const vector<vector<int>>& nums) {
    using T = tuple<int,int,int>; // val, row, col
    priority_queue<T, vector<T>, greater<T>> pq;
    int curMax = INT_MIN;
    for (int i = 0; i < (int)nums.size(); ++i) {
        pq.push({nums[i][0], i, 0});
        curMax = max(curMax, nums[i][0]);
    }
    vector<int> bestRange = {0, INT_MAX};
    while (!pq.empty()) {
        auto [val, r, c] = pq.top(); pq.pop();
        if (curMax - val < bestRange[1] - bestRange[0])
            bestRange = {val, curMax};
        if (c + 1 == (int)nums[r].size()) break; // exhausted one list
        int nextVal = nums[r][c + 1];
        curMax = max(curMax, nextVal);
        pq.push({nextVal, r, c + 1});
    }
    return bestRange;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Merge Two Sorted Arrays  [Difficulty: Easy]
// Source: Classic (base case of k-way)
// ─────────────────────────────────────────────
// Approach: Two-pointer merge.
// Time: O(m+n)  Space: O(m+n)
vector<int> mergeTwoSortedArrays(const vector<int>& a, const vector<int>& b) {
    vector<int> result;
    int i = 0, j = 0;
    while (i < (int)a.size() && j < (int)b.size())
        result.push_back(a[i] <= b[j] ? a[i++] : b[j++]);
    while (i < (int)a.size()) result.push_back(a[i++]);
    while (j < (int)b.size()) result.push_back(b[j++]);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Find K-th Smallest Pair Sum  [Difficulty: Hard]
// Source: Classic / LeetCode 786
// ─────────────────────────────────────────────
// Approach: For sorted arrays, binary search on the answer; count pairs <= mid.
// Source: LeetCode 786 (K-th Smallest Prime Fraction — principle)
// Time: O(n log(max²))  Space: O(1)
// Problem: Given two sorted arrays, find k-th smallest pair sum
int kthSmallestPairSum(const vector<int>& A, const vector<int>& B, int k) {
    int n = (int)A.size(), m = (int)B.size();
    int lo = A[0] + B[0], hi = A[n-1] + B[m-1];
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        // Count pairs with sum <= mid
        int count = 0, j = m - 1;
        for (int i = 0; i < n; ++i) {
            while (j >= 0 && A[i] + B[j] > mid) --j;
            count += j + 1;
        }
        if (count >= k) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// ─────────────────────────────────────────────
// PROBLEM 6: K-th Smallest in Sorted Matrix  [Difficulty: Medium]
// Source: LeetCode 378
// ─────────────────────────────────────────────
// Approach: Treat each row as a sorted list; k-way merge, pop k times.
// Time: O(k log n)  Space: O(n)
int kthSmallestMatrix(const vector<vector<int>>& matrix, int k) {
    int n = (int)matrix.size();
    using T = tuple<int,int,int>;
    priority_queue<T, vector<T>, greater<T>> pq;
    for (int r = 0; r < n; ++r) pq.push({matrix[r][0], r, 0});
    int result = 0;
    for (int i = 0; i < k; ++i) {
        auto [val, r, c] = pq.top(); pq.pop();
        result = val;
        if (c + 1 < n) pq.push({matrix[r][c+1], r, c+1});
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Merge K Sorted Arrays  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Min-heap with (val, array_idx, element_idx).
// Time: O(n log k)  Space: O(k)
vector<int> mergeKArrays(const vector<vector<int>>& arrays) {
    using T = tuple<int,int,int>;
    priority_queue<T, vector<T>, greater<T>> pq;
    for (int i = 0; i < (int)arrays.size(); ++i)
        if (!arrays[i].empty()) pq.push({arrays[i][0], i, 0});
    vector<int> result;
    while (!pq.empty()) {
        auto [val, r, c] = pq.top(); pq.pop();
        result.push_back(val);
        if (c + 1 < (int)arrays[r].size()) pq.push({arrays[r][c+1], r, c+1});
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 8: K-th Largest Element in a Stream  [Difficulty: Easy]
// Source: LeetCode 703
// ─────────────────────────────────────────────
// Approach: Min-heap of size k; top = k-th largest.
// Time: O(log k) per add  Space: O(k)
class KthLargest {
    priority_queue<int, vector<int>, greater<int>> pq;
    int k;
public:
    KthLargest(int k, const vector<int>& nums) : k(k) {
        for (int n : nums) add(n);
    }
    int add(int val) {
        pq.push(val);
        while ((int)pq.size() > k) pq.pop();
        return pq.top();
    }
};

int main() {
    // Problem 1
    auto lists = vector<ListNode*>{
        buildList({1,4,5}),
        buildList({1,3,4}),
        buildList({2,6})
    };
    printList(mergeKLists(lists)); // 1 1 2 3 4 4 5 6

    // Problem 2
    cout << kthSmallestInKArrays({{1,3,5},{2,4,6},{0,7,8}}, 4) << "\n"; // 3

    // Problem 3
    auto p3 = smallestRange({{4,10,15,24,26},{0,9,12,20},{5,18,22,30}});
    cout << "[" << p3[0] << "," << p3[1] << "]\n"; // [20,24]

    // Problem 4
    auto p4 = mergeTwoSortedArrays({1,3,5}, {2,4,6});
    for (int v : p4) cout << v << " "; cout << "\n"; // 1 2 3 4 5 6

    // Problem 5
    cout << kthSmallestPairSum({1,7,11}, {2,4,6}, 3) << "\n"; // 7

    // Problem 6
    cout << kthSmallestMatrix({{1,5,9},{10,11,13},{12,13,15}}, 8) << "\n"; // 13

    // Problem 7
    auto p7 = mergeKArrays({{1,4,7},{2,5,8},{3,6,9}});
    for (int v : p7) cout << v << " "; cout << "\n"; // 1..9

    // Problem 8
    KthLargest kl(3, {4,5,8,2});
    cout << kl.add(3)  << "\n"; // 4
    cout << kl.add(5)  << "\n"; // 5
    cout << kl.add(10) << "\n"; // 5

    return 0;
}
