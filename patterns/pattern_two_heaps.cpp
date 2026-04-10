/*
 * PATTERN: Two Heaps
 *
 * CONCEPT:
 * Maintain two heaps: a max-heap for the lower half of data and a min-heap
 * for the upper half. Rebalancing after each insertion keeps sizes equal (or
 * one off). The median is always at the tops of the heaps. Useful any time
 * you need to track medians, balance, or find the k-th smallest/largest in
 * a streaming context.
 *
 * TIME:  O(log n) per insertion/query
 * SPACE: O(n)
 *
 * WHEN TO USE:
 * - Median of a data stream
 * - Sliding window median
 * - "Scheduling" problems where you need both extremes
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Find Median from Data Stream  [Difficulty: Hard]
// Source: LeetCode 295
// ─────────────────────────────────────────────
// Approach: maxHeap (lower half) + minHeap (upper half); keep balanced.
// Time: O(log n) add, O(1) findMedian   Space: O(n)
class MedianFinder {
    priority_queue<int> maxHeap;                        // lower half
    priority_queue<int, vector<int>, greater<int>> minHeap; // upper half
public:
    void addNum(int num) {
        maxHeap.push(num);
        minHeap.push(maxHeap.top()); maxHeap.pop();
        if (minHeap.size() > maxHeap.size()) {
            maxHeap.push(minHeap.top()); minHeap.pop();
        }
    }
    double findMedian() const {
        if (maxHeap.size() > minHeap.size()) return maxHeap.top();
        return (maxHeap.top() + minHeap.top()) / 2.0;
    }
};

// ─────────────────────────────────────────────
// PROBLEM 2: Sliding Window Median  [Difficulty: Hard]
// Source: LeetCode 480
// ─────────────────────────────────────────────
// Approach: Two heaps with lazy deletion via an "invalidated" map.
// Time: O(n log k)  Space: O(k)
vector<double> medianSlidingWindow(const vector<int>& nums, int k) {
    priority_queue<int> lo;                                 // max-heap (lower half)
    priority_queue<int, vector<int>, greater<int>> hi;     // min-heap (upper half)
    unordered_map<int, int> invalidated;
    int loSize = 0, hiSize = 0;

    auto rebalance = [&]() {
        if (loSize > hiSize + 1) { hi.push(lo.top()); lo.pop(); --loSize; ++hiSize; }
        if (hiSize > loSize)     { lo.push(hi.top()); hi.pop(); ++loSize; --hiSize; }
    };
    auto getTop = [&](auto& heap, unordered_map<int,int>& inv) {
        while (!heap.empty() && inv.count(heap.top()) && inv[heap.top()] > 0) {
            --inv[heap.top()]; heap.pop();
        }
    };

    // Initialize first window
    for (int i = 0; i < k; ++i) { lo.push(nums[i]); ++loSize; }
    for (int i = 0; i < k / 2; ++i) { hi.push(lo.top()); lo.pop(); --loSize; ++hiSize; }

    vector<double> result;
    auto getMedian = [&]() -> double {
        getTop(lo, invalidated); getTop(hi, invalidated);
        if (k % 2 == 1) return lo.top();
        return ((double)lo.top() + hi.top()) / 2.0;
    };

    result.push_back(getMedian());

    for (int i = k; i < (int)nums.size(); ++i) {
        int outgoing = nums[i - k], incoming = nums[i];
        // Add incoming
        getTop(lo, invalidated);
        if (incoming <= lo.top()) { lo.push(incoming); ++loSize; }
        else                      { hi.push(incoming); ++hiSize; }
        // Invalidate outgoing
        ++invalidated[outgoing];
        getTop(lo, invalidated); getTop(hi, invalidated);
        if (outgoing <= lo.top()) --loSize; else --hiSize;
        rebalance();
        result.push_back(getMedian());
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: IPO / Maximize Capital  [Difficulty: Hard]
// Source: LeetCode 502
// ─────────────────────────────────────────────
// Approach: Min-heap by capital (lock); max-heap by profit (unlock when affordable).
// Time: O(n log n)  Space: O(n)
int findMaximizedCapital(int k, int w,
                         const vector<int>& profits,
                         const vector<int>& capital) {
    int n = (int)profits.size();
    vector<pair<int,int>> projects(n);
    for (int i = 0; i < n; ++i) projects[i] = {capital[i], profits[i]};
    sort(projects.begin(), projects.end());

    priority_queue<int> maxProfit; // affordable projects
    int i = 0;
    for (int j = 0; j < k; ++j) {
        while (i < n && projects[i].first <= w) {
            maxProfit.push(projects[i++].second);
        }
        if (maxProfit.empty()) break;
        w += maxProfit.top(); maxProfit.pop();
    }
    return w;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Task Scheduler  [Difficulty: Medium]
// Source: LeetCode 621
// ─────────────────────────────────────────────
// Approach: Max-heap of counts; simulate CPU rounds with cooldown n.
// Time: O(time)  Space: O(26)
int leastInterval(const vector<char>& tasks, int n) {
    vector<int> freq(26, 0);
    for (char c : tasks) ++freq[c - 'A'];
    priority_queue<int> pq(freq.begin(), freq.end());
    int time = 0;
    while (!pq.empty()) {
        vector<int> temp;
        int cycles = min(n + 1, (int)pq.size() + n); // up to n+1 tasks per round
        int cycle = 0;
        while (cycle <= n) {
            if (!pq.empty()) {
                temp.push_back(pq.top() - 1); pq.pop();
            }
            ++time; ++cycle;
            if (pq.empty() && temp.empty()) break;
        }
        for (int cnt : temp) if (cnt > 0) pq.push(cnt);
    }
    return time;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Meeting Rooms III (Assign tasks to rooms)  [Difficulty: Hard]
// Source: LeetCode 2402
// ─────────────────────────────────────────────
// Approach: Min-heap of available room IDs; min-heap of (end_time, room_id) for busy.
// Time: O(n log n)  Space: O(n)
int mostBooked(int n, vector<vector<int>> meetings) {
    sort(meetings.begin(), meetings.end());
    priority_queue<int, vector<int>, greater<int>> available; // room IDs
    priority_queue<pair<long long,int>, vector<pair<long long,int>>, greater<>> busy;
    for (int i = 0; i < n; ++i) available.push(i);
    vector<int> count(n, 0);

    for (auto& m : meetings) {
        long long start = m[0], end = m[1];
        // Free rooms that finished before current meeting
        while (!busy.empty() && busy.top().first <= start) {
            available.push(busy.top().second); busy.pop();
        }
        if (!available.empty()) {
            int room = available.top(); available.pop();
            ++count[room];
            busy.push({end, room});
        } else {
            // Delay to earliest available room
            auto [endTime, room] = busy.top(); busy.pop();
            ++count[room];
            busy.push({endTime + (end - start), room});
        }
    }
    return max_element(count.begin(), count.end()) - count.begin();
}

// ─────────────────────────────────────────────
// PROBLEM 6: K Closest Points to Origin  [Difficulty: Medium]
// Source: LeetCode 973
// ─────────────────────────────────────────────
// Approach: Max-heap of size k; maintain closest k points.
// Time: O(n log k)  Space: O(k)
vector<vector<int>> kClosest(const vector<vector<int>>& points, int k) {
    auto dist = [](const vector<int>& p) { return p[0]*p[0] + p[1]*p[1]; };
    priority_queue<pair<int,int>> pq; // (dist, index)
    for (int i = 0; i < (int)points.size(); ++i) {
        pq.push({dist(points[i]), i});
        if ((int)pq.size() > k) pq.pop();
    }
    vector<vector<int>> result;
    while (!pq.empty()) { result.push_back(points[pq.top().second]); pq.pop(); }
    return result;
}

int main() {
    // Problem 1
    MedianFinder mf;
    mf.addNum(1); mf.addNum(2);
    cout << mf.findMedian() << "\n"; // 1.5
    mf.addNum(3);
    cout << mf.findMedian() << "\n"; // 2.0

    // Problem 2
    auto p2 = medianSlidingWindow({1,3,-1,-3,5,3,6,7}, 3);
    for (double v : p2) cout << v << " "; cout << "\n"; // 1 -1 -1 3 5 6

    // Problem 3
    cout << findMaximizedCapital(2, 0, {1,2,3}, {0,1,1}) << "\n"; // 4

    // Problem 4
    cout << leastInterval({'A','A','A','B','B','B'}, 2) << "\n"; // 8

    // Problem 5
    cout << mostBooked(2, {{0,10},{1,5},{2,7},{3,4}}) << "\n"; // 0

    // Problem 6
    auto p6 = kClosest({{1,3},{-2,2}}, 1);
    for (auto& p : p6) cout << "[" << p[0] << "," << p[1] << "] "; cout << "\n"; // [-2,2]

    return 0;
}
