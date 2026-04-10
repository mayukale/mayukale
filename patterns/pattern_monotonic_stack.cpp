/*
 * PATTERN: Monotonic Stack / Queue
 *
 * CONCEPT:
 * A stack maintained in strictly increasing or decreasing order of values.
 * When a new element violates the order, pop until it doesn't — those popped
 * elements have found their "next greater/smaller" answer. Monotonic deques
 * extend this to sliding window max/min in O(1) per element.
 *
 * TIME:  O(n) — each element pushed and popped at most once
 * SPACE: O(n)
 *
 * WHEN TO USE:
 * - Next greater/smaller element (to the right or left)
 * - Largest rectangle in histogram
 * - Sliding window maximum/minimum
 * - Sum of subarray minimums/maximums
 * - Stock span, temperature problems
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Next Greater Element I  [Difficulty: Easy]
// Source: LeetCode 496
// ─────────────────────────────────────────────
// Approach: Monotonic decreasing stack on nums2; map each element to its NGE.
// Time: O(n+m)  Space: O(n)
vector<int> nextGreaterElement(const vector<int>& nums1, const vector<int>& nums2) {
    unordered_map<int, int> nge;
    stack<int> st;
    for (int n : nums2) {
        while (!st.empty() && st.top() < n) { nge[st.top()] = n; st.pop(); }
        st.push(n);
    }
    vector<int> result;
    for (int n : nums1) result.push_back(nge.count(n) ? nge[n] : -1);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Next Greater Element II (circular)  [Difficulty: Medium]
// Source: LeetCode 503
// ─────────────────────────────────────────────
// Approach: Process array twice (simulate circular); decreasing stack of indices.
// Time: O(n)  Space: O(n)
vector<int> nextGreaterElements(const vector<int>& nums) {
    int n = (int)nums.size();
    vector<int> result(n, -1);
    stack<int> st;
    for (int i = 0; i < 2 * n; ++i) {
        while (!st.empty() && nums[st.top()] < nums[i % n]) {
            result[st.top()] = nums[i % n]; st.pop();
        }
        if (i < n) st.push(i);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Daily Temperatures  [Difficulty: Medium]
// Source: LeetCode 739
// ─────────────────────────────────────────────
// Approach: Decreasing stack of indices; when warmer day found, compute gap.
// Time: O(n)  Space: O(n)
vector<int> dailyTemperatures(const vector<int>& T) {
    int n = (int)T.size();
    vector<int> result(n, 0);
    stack<int> st;
    for (int i = 0; i < n; ++i) {
        while (!st.empty() && T[st.top()] < T[i]) {
            result[st.top()] = i - st.top(); st.pop();
        }
        st.push(i);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Largest Rectangle in Histogram  [Difficulty: Hard]
// Source: LeetCode 84
// ─────────────────────────────────────────────
// Approach: Increasing stack of indices; on pop, compute width to current right boundary.
// Time: O(n)  Space: O(n)
int largestRectangleArea(vector<int> heights) {
    heights.push_back(0); // sentinel
    stack<int> st;
    int maxArea = 0;
    for (int i = 0; i < (int)heights.size(); ++i) {
        while (!st.empty() && heights[st.top()] > heights[i]) {
            int h = heights[st.top()]; st.pop();
            int w = st.empty() ? i : i - st.top() - 1;
            maxArea = max(maxArea, h * w);
        }
        st.push(i);
    }
    return maxArea;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Maximal Rectangle  [Difficulty: Hard]
// Source: LeetCode 85
// ─────────────────────────────────────────────
// Approach: Build histogram row by row; apply largestRectangleArea to each row.
// Time: O(m*n)  Space: O(n)
int maximalRectangle(const vector<vector<char>>& matrix) {
    if (matrix.empty()) return 0;
    int n = (int)matrix[0].size(), maxArea = 0;
    vector<int> heights(n, 0);
    for (auto& row : matrix) {
        for (int c = 0; c < n; ++c)
            heights[c] = (row[c] == '1') ? heights[c] + 1 : 0;
        maxArea = max(maxArea, largestRectangleArea(heights));
    }
    return maxArea;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Trapping Rain Water  [Difficulty: Hard]
// Source: LeetCode 42
// ─────────────────────────────────────────────
// Approach: Stack holds indices; when taller bar found, compute water in the bowl.
// Time: O(n)  Space: O(n)
int trap(const vector<int>& height) {
    stack<int> st;
    int water = 0;
    for (int i = 0; i < (int)height.size(); ++i) {
        while (!st.empty() && height[st.top()] < height[i]) {
            int bot = st.top(); st.pop();
            if (st.empty()) break;
            int w = i - st.top() - 1;
            int h = min(height[st.top()], height[i]) - height[bot];
            water += w * h;
        }
        st.push(i);
    }
    return water;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Sum of Subarray Minimums  [Difficulty: Medium]
// Source: LeetCode 907
// ─────────────────────────────────────────────
// Approach: For each element, find previous/next less element using two stacks.
//           Contribution = arr[i] * (i - left) * (right - i).
// Time: O(n)  Space: O(n)
int sumSubarrayMins(const vector<int>& arr) {
    const int MOD = 1e9 + 7;
    int n = (int)arr.size();
    vector<int> left(n), right(n);
    stack<int> st;
    // Previous less (strict)
    for (int i = 0; i < n; ++i) {
        while (!st.empty() && arr[st.top()] >= arr[i]) st.pop();
        left[i] = st.empty() ? i + 1 : i - st.top();
        st.push(i);
    }
    while (!st.empty()) st.pop();
    // Next less or equal
    for (int i = n - 1; i >= 0; --i) {
        while (!st.empty() && arr[st.top()] > arr[i]) st.pop();
        right[i] = st.empty() ? n - i : st.top() - i;
        st.push(i);
    }
    long long sum = 0;
    for (int i = 0; i < n; ++i)
        sum = (sum + (long long)arr[i] * left[i] % MOD * right[i]) % MOD;
    return (int)sum;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Sliding Window Maximum (Monotonic Deque)  [Difficulty: Hard]
// Source: LeetCode 239
// ─────────────────────────────────────────────
// Approach: Deque stores indices in decreasing order of value; front = window max.
// Time: O(n)  Space: O(k)
vector<int> maxSlidingWindow(const vector<int>& nums, int k) {
    deque<int> dq;
    vector<int> result;
    for (int i = 0; i < (int)nums.size(); ++i) {
        if (!dq.empty() && dq.front() <= i - k) dq.pop_front();
        while (!dq.empty() && nums[dq.back()] < nums[i]) dq.pop_back();
        dq.push_back(i);
        if (i >= k - 1) result.push_back(nums[dq.front()]);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Stock Span Problem  [Difficulty: Medium]
// Source: LeetCode 901
// ─────────────────────────────────────────────
// Approach: Stack of (price, span); merge consecutive smaller prices.
// Time: O(1) amortized per call  Space: O(n)
class StockSpanner {
    stack<pair<int,int>> st; // (price, span)
public:
    int next(int price) {
        int span = 1;
        while (!st.empty() && st.top().first <= price) {
            span += st.top().second; st.pop();
        }
        st.push({price, span});
        return span;
    }
};

// ─────────────────────────────────────────────
// PROBLEM 10: 132 Pattern  [Difficulty: Medium]
// Source: LeetCode 456
// ─────────────────────────────────────────────
// Approach: Scan right-to-left; stack maintains candidates for "2" (s3);
//           track running min for "1" (s1); if current > s3 -> pattern found.
// Time: O(n)  Space: O(n)
bool find132pattern(const vector<int>& nums) {
    stack<int> st;
    int s3 = INT_MIN; // the "2" element (middle in 132)
    for (int i = (int)nums.size() - 1; i >= 0; --i) {
        if (nums[i] < s3) return true;
        while (!st.empty() && st.top() < nums[i]) { s3 = st.top(); st.pop(); }
        st.push(nums[i]);
    }
    return false;
}

// ─────────────────────────────────────────────
// PROBLEM 11: Remove K Digits  [Difficulty: Medium]
// Source: LeetCode 402
// ─────────────────────────────────────────────
// Approach: Greedy with monotonic increasing stack; pop larger digits when k remains.
// Time: O(n)  Space: O(n)
string removeKdigits(const string& num, int k) {
    string st;
    for (char c : num) {
        while (k > 0 && !st.empty() && st.back() > c) { st.pop_back(); --k; }
        st += c;
    }
    while (k-- > 0) st.pop_back();
    // Remove leading zeros
    int start = 0;
    while (start < (int)st.size() - 1 && st[start] == '0') ++start;
    return st.empty() ? "0" : st.substr(start);
}

int main() {
    // Problem 1
    auto p1 = nextGreaterElement({4,1,2}, {1,3,4,2});
    for (int v : p1) cout << v << " "; cout << "\n"; // -1 3 -1

    // Problem 2
    auto p2 = nextGreaterElements({1,2,1});
    for (int v : p2) cout << v << " "; cout << "\n"; // 2 -1 2

    // Problem 3
    auto p3 = dailyTemperatures({73,74,75,71,69,72,76,73});
    for (int v : p3) cout << v << " "; cout << "\n"; // 1 1 4 2 1 1 0 0

    // Problem 4
    cout << largestRectangleArea({2,1,5,6,2,3}) << "\n"; // 10

    // Problem 5
    cout << maximalRectangle({{'1','0','1','0','0'},
                               {'1','0','1','1','1'},
                               {'1','1','1','1','1'},
                               {'1','0','0','1','0'}}) << "\n"; // 6

    // Problem 6
    cout << trap({0,1,0,2,1,0,1,3,2,1,2,1}) << "\n"; // 6

    // Problem 7
    cout << sumSubarrayMins({3,1,2,4}) << "\n"; // 17

    // Problem 8
    auto p8 = maxSlidingWindow({1,3,-1,-3,5,3,6,7}, 3);
    for (int v : p8) cout << v << " "; cout << "\n"; // 3 3 5 5 6 7

    // Problem 9
    StockSpanner ss;
    cout << ss.next(100) << " " << ss.next(80) << " " << ss.next(60) << " "
         << ss.next(70)  << " " << ss.next(60) << " " << ss.next(75) << " "
         << ss.next(85)  << "\n"; // 1 1 1 2 1 4 6

    // Problem 10
    cout << boolalpha << find132pattern({3,1,4,2}) << "\n"; // true

    // Problem 11
    cout << removeKdigits("1432219", 3) << "\n"; // 1219

    return 0;
}
