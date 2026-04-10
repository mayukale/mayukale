/*
 * PATTERN: Modified Binary Search
 *
 * CONCEPT:
 * Extend classic binary search to handle rotated arrays, unknown-length arrays,
 * "find first/last occurrence", "search in a matrix", or "minimize/maximize a
 * value subject to a predicate". The invariant shifts, but the O(log n) bound
 * is preserved by halving the search space each iteration.
 *
 * TIME:  O(log n)
 * SPACE: O(1)
 *
 * WHEN TO USE:
 * - Sorted (or partially sorted) array with a find/count query
 * - "Find minimum/maximum that satisfies a condition" (binary search on answer)
 * - Rotated sorted array, mountain array, bitonic array
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Binary Search (classic)  [Difficulty: Easy]
// Source: LeetCode 704
// ─────────────────────────────────────────────
// Approach: Standard bisection.
// Time: O(log n)  Space: O(1)
int binarySearch(const vector<int>& nums, int target) {
    int lo = 0, hi = (int)nums.size() - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (nums[mid] == target) return mid;
        else if (nums[mid] < target) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Find First and Last Position  [Difficulty: Medium]
// Source: LeetCode 34
// ─────────────────────────────────────────────
// Approach: Two binary searches: lower_bound and upper_bound-1.
// Time: O(log n)  Space: O(1)
vector<int> searchRange(const vector<int>& nums, int target) {
    auto lo = lower_bound(nums.begin(), nums.end(), target);
    if (lo == nums.end() || *lo != target) return {-1, -1};
    auto hi = upper_bound(nums.begin(), nums.end(), target);
    return {(int)(lo - nums.begin()), (int)(hi - nums.begin()) - 1};
}

// ─────────────────────────────────────────────
// PROBLEM 3: Search in Rotated Sorted Array  [Difficulty: Medium]
// Source: LeetCode 33
// ─────────────────────────────────────────────
// Approach: Identify which half is sorted; determine if target is in that half.
// Time: O(log n)  Space: O(1)
int searchRotated(const vector<int>& nums, int target) {
    int lo = 0, hi = (int)nums.size() - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (nums[mid] == target) return mid;
        if (nums[lo] <= nums[mid]) { // left half sorted
            if (target >= nums[lo] && target < nums[mid]) hi = mid - 1;
            else lo = mid + 1;
        } else { // right half sorted
            if (target > nums[mid] && target <= nums[hi]) lo = mid + 1;
            else hi = mid - 1;
        }
    }
    return -1;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Find Minimum in Rotated Sorted Array  [Difficulty: Medium]
// Source: LeetCode 153
// ─────────────────────────────────────────────
// Approach: If nums[mid] > nums[hi], minimum is in right half; else left.
// Time: O(log n)  Space: O(1)
int findMin(const vector<int>& nums) {
    int lo = 0, hi = (int)nums.size() - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (nums[mid] > nums[hi]) lo = mid + 1;
        else hi = mid;
    }
    return nums[lo];
}

// ─────────────────────────────────────────────
// PROBLEM 5: Peak Index in Mountain Array  [Difficulty: Easy]
// Source: LeetCode 852
// ─────────────────────────────────────────────
// Approach: Move toward the rising slope (mid < mid+1 → go right).
// Time: O(log n)  Space: O(1)
int peakIndexInMountainArray(const vector<int>& arr) {
    int lo = 0, hi = (int)arr.size() - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid] < arr[mid + 1]) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Search a 2D Matrix  [Difficulty: Medium]
// Source: LeetCode 74
// ─────────────────────────────────────────────
// Approach: Treat flattened matrix as sorted array; index math for row/col.
// Time: O(log(m*n))  Space: O(1)
bool searchMatrix(const vector<vector<int>>& matrix, int target) {
    int m = (int)matrix.size(), n = (int)matrix[0].size();
    int lo = 0, hi = m * n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int val = matrix[mid / n][mid % n];
        if (val == target) return true;
        else if (val < target) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Kth Smallest Element in Sorted Matrix  [Difficulty: Medium]
// Source: LeetCode 378
// ─────────────────────────────────────────────
// Approach: Binary search on value; count elements <= mid; find k-th.
// Time: O(n log(max-min))  Space: O(1)
int kthSmallest(const vector<vector<int>>& matrix, int k) {
    int n = (int)matrix.size();
    int lo = matrix[0][0], hi = matrix[n-1][n-1];
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int count = 0, j = n - 1;
        for (int i = 0; i < n; ++i) {
            while (j >= 0 && matrix[i][j] > mid) --j;
            count += j + 1;
        }
        if (count < k) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Find the Square Root (floor)  [Difficulty: Easy]
// Source: LeetCode 69
// ─────────────────────────────────────────────
// Approach: Binary search on answer in [0..x].
// Time: O(log x)  Space: O(1)
int mySqrt(int x) {
    if (x < 2) return x;
    long lo = 1, hi = x / 2;
    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;
        if (mid * mid == x) return (int)mid;
        else if (mid * mid < x) lo = mid + 1;
        else hi = mid - 1;
    }
    return (int)hi;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Capacity to Ship Packages Within D Days  [Difficulty: Medium]
// Source: LeetCode 1011
// ─────────────────────────────────────────────
// Approach: Binary search on capacity; check feasibility with greedy simulation.
// Time: O(n log(sum))  Space: O(1)
bool canShip(const vector<int>& weights, int days, int cap) {
    int d = 1, cur = 0;
    for (int w : weights) {
        if (cur + w > cap) { ++d; cur = 0; }
        cur += w;
    }
    return d <= days;
}
int shipWithinDays(const vector<int>& weights, int days) {
    int lo = *max_element(weights.begin(), weights.end());
    int hi = accumulate(weights.begin(), weights.end(), 0);
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (canShip(weights, days, mid)) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Koko Eating Bananas  [Difficulty: Medium]
// Source: LeetCode 875
// ─────────────────────────────────────────────
// Approach: Binary search on eating speed; check total hours <= h.
// Time: O(n log(max_pile))  Space: O(1)
int minEatingSpeed(const vector<int>& piles, int h) {
    int lo = 1, hi = *max_element(piles.begin(), piles.end());
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        long hours = 0;
        for (int p : piles) hours += (p + mid - 1) / mid;
        if (hours <= h) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// ─────────────────────────────────────────────
// PROBLEM 11: Find K-th Smallest Pair Distance  [Difficulty: Hard]
// Source: LeetCode 719
// ─────────────────────────────────────────────
// Approach: Sort; binary search on distance; count pairs with distance <= mid.
// Time: O(n log n + n log(max-min))  Space: O(1)
int smallestDistancePair(vector<int> nums, int k) {
    sort(nums.begin(), nums.end());
    int n = (int)nums.size();
    int lo = 0, hi = nums[n-1] - nums[0];
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int count = 0, left = 0;
        for (int right = 0; right < n; ++right) {
            while (nums[right] - nums[left] > mid) ++left;
            count += right - left;
        }
        if (count >= k) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

int main() {
    // Problem 1
    cout << binarySearch({-1,0,3,5,9,12}, 9) << "\n"; // 4

    // Problem 2
    auto p2 = searchRange({5,7,7,8,8,10}, 8);
    cout << p2[0] << " " << p2[1] << "\n"; // 3 4

    // Problem 3
    cout << searchRotated({4,5,6,7,0,1,2}, 0) << "\n"; // 4

    // Problem 4
    cout << findMin({3,4,5,1,2}) << "\n"; // 1

    // Problem 5
    cout << peakIndexInMountainArray({0,2,1,0}) << "\n"; // 1

    // Problem 6
    cout << boolalpha << searchMatrix({{1,3,5,7},{10,11,16,20},{23,30,34,60}}, 3) << "\n"; // true

    // Problem 7
    cout << kthSmallest({{1,5,9},{10,11,13},{12,13,15}}, 8) << "\n"; // 13

    // Problem 8
    cout << mySqrt(8) << "\n"; // 2

    // Problem 9
    cout << shipWithinDays({1,2,3,4,5,6,7,8,9,10}, 5) << "\n"; // 15

    // Problem 10
    cout << minEatingSpeed({3,6,7,11}, 8) << "\n"; // 4

    // Problem 11
    cout << smallestDistancePair({1,3,1}, 1) << "\n"; // 0

    return 0;
}
