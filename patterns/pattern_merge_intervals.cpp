/*
 * PATTERN: Merge Intervals
 *
 * CONCEPT:
 * Sort intervals by start time, then greedily merge overlapping ones.
 * Overlapping means next.start <= current.end. After merging, gaps mean
 * no overlap. Can also be used for insertion, intersection, and scheduling.
 *
 * TIME:  O(n log n) for sort, O(n) for merge
 * SPACE: O(n) output
 *
 * WHEN TO USE:
 * - "Merge overlapping intervals"
 * - "Insert an interval into a sorted list"
 * - "Find gaps / free time"
 * - "Minimum rooms / platforms needed"
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Merge Intervals  [Difficulty: Medium]
// Source: LeetCode 56
// ─────────────────────────────────────────────
// Approach: Sort by start; extend current end if overlap, else push.
// Time: O(n log n)  Space: O(n)
vector<vector<int>> merge(vector<vector<int>> intervals) {
    sort(intervals.begin(), intervals.end());
    vector<vector<int>> merged;
    for (auto& iv : intervals) {
        if (!merged.empty() && iv[0] <= merged.back()[1])
            merged.back()[1] = max(merged.back()[1], iv[1]);
        else
            merged.push_back(iv);
    }
    return merged;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Insert Interval  [Difficulty: Medium]
// Source: LeetCode 57
// ─────────────────────────────────────────────
// Approach: Skip non-overlapping before, merge overlapping, append rest.
// Time: O(n)  Space: O(n)
vector<vector<int>> insert(vector<vector<int>> intervals, vector<int> newInterval) {
    vector<vector<int>> result;
    int i = 0, n = (int)intervals.size();
    // Add all intervals ending before newInterval starts
    while (i < n && intervals[i][1] < newInterval[0])
        result.push_back(intervals[i++]);
    // Merge all overlapping
    while (i < n && intervals[i][0] <= newInterval[1]) {
        newInterval[0] = min(newInterval[0], intervals[i][0]);
        newInterval[1] = max(newInterval[1], intervals[i][1]);
        ++i;
    }
    result.push_back(newInterval);
    // Add remaining
    while (i < n) result.push_back(intervals[i++]);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Interval List Intersections  [Difficulty: Medium]
// Source: LeetCode 986
// ─────────────────────────────────────────────
// Approach: Two pointers; intersection is [max(s), min(e)]; advance the earlier-ending.
// Time: O(m+n)  Space: O(1)
vector<vector<int>> intervalIntersection(
    const vector<vector<int>>& A, const vector<vector<int>>& B)
{
    vector<vector<int>> result;
    int i = 0, j = 0;
    while (i < (int)A.size() && j < (int)B.size()) {
        int lo = max(A[i][0], B[j][0]);
        int hi = min(A[i][1], B[j][1]);
        if (lo <= hi) result.push_back({lo, hi});
        if (A[i][1] < B[j][1]) ++i; else ++j;
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Meeting Rooms I  [Difficulty: Easy]
// Source: LeetCode 252
// ─────────────────────────────────────────────
// Approach: Sort by start; if next start < prev end, conflict.
// Time: O(n log n)  Space: O(1)
bool canAttendMeetings(vector<vector<int>> intervals) {
    sort(intervals.begin(), intervals.end());
    for (int i = 1; i < (int)intervals.size(); ++i)
        if (intervals[i][0] < intervals[i-1][1]) return false;
    return true;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Meeting Rooms II (Min Rooms)  [Difficulty: Medium]
// Source: LeetCode 253
// ─────────────────────────────────────────────
// Approach: Min-heap of end times; if top <= new start, reuse room.
// Time: O(n log n)  Space: O(n)
int minMeetingRooms(vector<vector<int>> intervals) {
    sort(intervals.begin(), intervals.end());
    priority_queue<int, vector<int>, greater<int>> minHeap; // end times
    for (auto& iv : intervals) {
        if (!minHeap.empty() && minHeap.top() <= iv[0])
            minHeap.pop();
        minHeap.push(iv[1]);
    }
    return (int)minHeap.size();
}

// ─────────────────────────────────────────────
// PROBLEM 6: Non-Overlapping Intervals  [Difficulty: Medium]
// Source: LeetCode 435
// ─────────────────────────────────────────────
// Approach: Sort by end; greedy — keep interval with earliest end.
// Time: O(n log n)  Space: O(1)
int eraseOverlapIntervals(vector<vector<int>> intervals) {
    if (intervals.empty()) return 0;
    sort(intervals.begin(), intervals.end(),
         [](const auto& a, const auto& b){ return a[1] < b[1]; });
    int removed = 0, end = intervals[0][1];
    for (int i = 1; i < (int)intervals.size(); ++i) {
        if (intervals[i][0] < end) ++removed;
        else end = intervals[i][1];
    }
    return removed;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Minimum Number of Arrows to Burst Balloons  [Difficulty: Medium]
// Source: LeetCode 452
// ─────────────────────────────────────────────
// Approach: Sort by end; one arrow bursts all overlapping from current end.
// Time: O(n log n)  Space: O(1)
int findMinArrowShots(vector<vector<int>> points) {
    sort(points.begin(), points.end(),
         [](const auto& a, const auto& b){ return a[1] < b[1]; });
    int arrows = 1;
    int arrowPos = points[0][1];
    for (int i = 1; i < (int)points.size(); ++i) {
        if (points[i][0] > arrowPos) {
            ++arrows;
            arrowPos = points[i][1];
        }
    }
    return arrows;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Employee Free Time  [Difficulty: Hard]
// Source: LeetCode 759
// ─────────────────────────────────────────────
// Approach: Flatten all schedules, sort, find gaps between merged intervals.
// Time: O(n log n)  Space: O(n)
// Using pair<int,int> for intervals
vector<pair<int,int>> employeeFreeTime(const vector<vector<pair<int,int>>>& schedule) {
    vector<pair<int,int>> all;
    for (auto& emp : schedule)
        for (auto& iv : emp) all.push_back(iv);
    sort(all.begin(), all.end());
    vector<pair<int,int>> gaps;
    int end = all[0].second;
    for (int i = 1; i < (int)all.size(); ++i) {
        if (all[i].first > end)
            gaps.push_back({end, all[i].first});
        end = max(end, all[i].second);
    }
    return gaps;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Car Pooling  [Difficulty: Medium]
// Source: LeetCode 1094
// ─────────────────────────────────────────────
// Approach: Difference array on stops [0..1000]; track running passenger count.
// Time: O(n + max_stop)  Space: O(max_stop)
bool carPooling(const vector<vector<int>>& trips, int capacity) {
    vector<int> stops(1001, 0);
    for (auto& t : trips) {
        stops[t[1]] += t[0];
        stops[t[2]] -= t[0];
    }
    int cur = 0;
    for (int s : stops) {
        cur += s;
        if (cur > capacity) return false;
    }
    return true;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Minimum Interval to Include Each Query  [Difficulty: Hard]
// Source: LeetCode 1851
// ─────────────────────────────────────────────
// Approach: Sort intervals by start and queries; min-heap of (size, end).
// Time: O((n+q) log n)  Space: O(n+q)
vector<int> minInterval(vector<vector<int>> intervals, vector<int> queries) {
    sort(intervals.begin(), intervals.end());
    int n = (int)queries.size();
    vector<int> idx(n);
    iota(idx.begin(), idx.end(), 0);
    sort(idx.begin(), idx.end(), [&](int a, int b){ return queries[a] < queries[b]; });
    // min-heap: (size, end)
    priority_queue<pair<int,int>, vector<pair<int,int>>, greater<>> pq;
    vector<int> ans(n, -1);
    int i = 0;
    for (int qi : idx) {
        int q = queries[qi];
        while (i < (int)intervals.size() && intervals[i][0] <= q) {
            int sz = intervals[i][1] - intervals[i][0] + 1;
            pq.push({sz, intervals[i][1]});
            ++i;
        }
        while (!pq.empty() && pq.top().second < q) pq.pop();
        if (!pq.empty()) ans[qi] = pq.top().first;
    }
    return ans;
}

int main() {
    // Problem 1
    auto m1 = merge({{1,3},{2,6},{8,10},{15,18}});
    for (auto& v : m1) cout << "[" << v[0] << "," << v[1] << "] "; cout << "\n";
    // [1,6] [8,10] [15,18]

    // Problem 2
    auto m2 = insert({{1,3},{6,9}}, {2,5});
    for (auto& v : m2) cout << "[" << v[0] << "," << v[1] << "] "; cout << "\n";
    // [1,5] [6,9]

    // Problem 3
    auto m3 = intervalIntersection({{0,2},{5,10},{13,23},{24,25}},
                                   {{1,5},{8,12},{15,24},{25,26}});
    for (auto& v : m3) cout << "[" << v[0] << "," << v[1] << "] "; cout << "\n";

    // Problem 4
    cout << boolalpha << canAttendMeetings({{0,30},{5,10},{15,20}}) << "\n"; // false

    // Problem 5
    cout << minMeetingRooms({{0,30},{5,10},{15,20}}) << "\n"; // 2

    // Problem 6
    cout << eraseOverlapIntervals({{1,2},{2,3},{3,4},{1,3}}) << "\n"; // 1

    // Problem 7
    cout << findMinArrowShots({{10,16},{2,8},{1,6},{7,12}}) << "\n"; // 2

    // Problem 8
    vector<vector<pair<int,int>>> sched = {{{1,3},{6,7}},{{2,4}},{{2,5},{9,12}}};
    auto gaps = employeeFreeTime(sched);
    for (auto& g : gaps) cout << "[" << g.first << "," << g.second << "] "; cout << "\n";

    // Problem 9
    cout << carPooling({{2,1,5},{3,3,7}}, 4) << "\n"; // false

    // Problem 10
    auto ans10 = minInterval({{1,4},{2,4},{3,6},{4,4}}, {2,3,4,5});
    for (int v : ans10) cout << v << " "; cout << "\n"; // 3 3 1 4

    return 0;
}
