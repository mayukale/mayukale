/*
 * PATTERN: Cyclic Sort
 *
 * CONCEPT:
 * When an array contains numbers in range [1..n] (or [0..n-1]), each number
 * belongs at a specific index. Swap each element to its correct position in
 * one pass. Then a second pass identifies missing/duplicate/misplaced numbers
 * in O(n) without extra space.
 *
 * TIME:  O(n)  — at most 2n swaps total
 * SPACE: O(1)
 *
 * WHEN TO USE:
 * - Array of n numbers in range [1..n] (or [0..n])
 * - Find missing number, duplicate number, or all missing/duplicates
 * - "First missing positive" style problems
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Cyclic Sort (sort range [1..n] in-place)  [Difficulty: Easy]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: While nums[i] != i+1, swap nums[i] to its correct index.
// Time: O(n)  Space: O(1)
void cyclicSort(vector<int>& nums) {
    int i = 0;
    while (i < (int)nums.size()) {
        int correct = nums[i] - 1;
        if (nums[i] != nums[correct]) swap(nums[i], nums[correct]);
        else ++i;
    }
}

// ─────────────────────────────────────────────
// PROBLEM 2: Find the Missing Number  [Difficulty: Easy]
// Source: LeetCode 268
// ─────────────────────────────────────────────
// Approach: Cyclic sort [0..n], then find index where nums[i] != i.
// Time: O(n)  Space: O(1)
int missingNumber(vector<int> nums) {
    int n = (int)nums.size(), i = 0;
    while (i < n) {
        int correct = nums[i];
        if (nums[i] < n && nums[i] != nums[correct]) swap(nums[i], nums[correct]);
        else ++i;
    }
    for (int j = 0; j < n; ++j)
        if (nums[j] != j) return j;
    return n;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Find All Numbers Disappeared in an Array  [Difficulty: Easy]
// Source: LeetCode 448
// ─────────────────────────────────────────────
// Approach: Cyclic sort [1..n], collect all indices where nums[i] != i+1.
// Time: O(n)  Space: O(1)
vector<int> findDisappearedNumbers(vector<int> nums) {
    int i = 0;
    while (i < (int)nums.size()) {
        int correct = nums[i] - 1;
        if (nums[i] != nums[correct]) swap(nums[i], nums[correct]);
        else ++i;
    }
    vector<int> missing;
    for (int j = 0; j < (int)nums.size(); ++j)
        if (nums[j] != j + 1) missing.push_back(j + 1);
    return missing;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Find the Duplicate Number  [Difficulty: Medium]
// Source: LeetCode 287
// ─────────────────────────────────────────────
// Approach: Cyclic sort; when nums[i] == nums[correct] and i != correct, duplicate found.
// Time: O(n)  Space: O(1)
int findDuplicate(vector<int> nums) {
    int i = 0;
    while (i < (int)nums.size()) {
        if (nums[i] != i + 1) {
            int correct = nums[i] - 1;
            if (nums[i] != nums[correct]) swap(nums[i], nums[correct]);
            else return nums[i];
        } else ++i;
    }
    return -1;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Find All Duplicates in an Array  [Difficulty: Medium]
// Source: LeetCode 442
// ─────────────────────────────────────────────
// Approach: Cyclic sort; all positions where nums[i] != i+1 are duplicates.
// Time: O(n)  Space: O(1)
vector<int> findAllDuplicates(vector<int> nums) {
    int i = 0;
    while (i < (int)nums.size()) {
        int correct = nums[i] - 1;
        if (nums[i] != nums[correct]) swap(nums[i], nums[correct]);
        else ++i;
    }
    vector<int> dups;
    for (int j = 0; j < (int)nums.size(); ++j)
        if (nums[j] != j + 1) dups.push_back(nums[j]);
    return dups;
}

// ─────────────────────────────────────────────
// PROBLEM 6: First Missing Positive  [Difficulty: Hard]
// Source: LeetCode 41
// ─────────────────────────────────────────────
// Approach: Ignore out-of-range values; cyclic sort [1..n]; scan for first gap.
// Time: O(n)  Space: O(1)
int firstMissingPositive(vector<int> nums) {
    int n = (int)nums.size(), i = 0;
    while (i < n) {
        int correct = nums[i] - 1;
        if (nums[i] > 0 && nums[i] <= n && nums[i] != nums[correct])
            swap(nums[i], nums[correct]);
        else ++i;
    }
    for (int j = 0; j < n; ++j)
        if (nums[j] != j + 1) return j + 1;
    return n + 1;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Find the Corrupt Pair (missing and duplicate)  [Difficulty: Easy]
// Source: Classic / GeeksforGeeks
// ─────────────────────────────────────────────
// Approach: Cyclic sort; index where nums[i] != i+1 gives duplicate and missing.
// Time: O(n)  Space: O(1)
// Returns {duplicate, missing}
pair<int,int> findCorruptPair(vector<int> nums) {
    int i = 0;
    while (i < (int)nums.size()) {
        int correct = nums[i] - 1;
        if (nums[i] != nums[correct]) swap(nums[i], nums[correct]);
        else ++i;
    }
    for (int j = 0; j < (int)nums.size(); ++j)
        if (nums[j] != j + 1) return {nums[j], j + 1};
    return {-1, -1};
}

// ─────────────────────────────────────────────
// PROBLEM 8: Find K Missing Positive Numbers  [Difficulty: Hard]
// Source: LeetCode 1539 / Classic extension
// ─────────────────────────────────────────────
// Approach: Cyclic sort, collect first k missing values from sorted+extra scan.
// Time: O(n + k)  Space: O(k)
vector<int> kMissingPositive(vector<int> nums, int k) {
    int i = 0;
    while (i < (int)nums.size()) {
        int correct = nums[i] - 1;
        if (nums[i] > 0 && nums[i] <= (int)nums.size() && nums[i] != nums[correct])
            swap(nums[i], nums[correct]);
        else ++i;
    }
    vector<int> missing;
    int extra = 1;
    for (int j = 0; j < (int)nums.size() && (int)missing.size() < k; ++j) {
        if (nums[j] != j + 1) {
            while (extra <= (int)nums.size() && (int)missing.size() < k) {
                if (extra != nums[j]) missing.push_back(extra);
                ++extra;
            }
        }
    }
    while ((int)missing.size() < k) missing.push_back(extra++);
    return missing;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Set Mismatch  [Difficulty: Easy]
// Source: LeetCode 645
// ─────────────────────────────────────────────
// Approach: Cyclic sort; duplicate at nums[i] != i+1, missing = i+1.
// Time: O(n)  Space: O(1)
// Returns {duplicate, missing}
vector<int> findErrorNums(vector<int> nums) {
    int i = 0;
    while (i < (int)nums.size()) {
        int correct = nums[i] - 1;
        if (nums[i] != nums[correct]) swap(nums[i], nums[correct]);
        else ++i;
    }
    for (int j = 0; j < (int)nums.size(); ++j)
        if (nums[j] != j + 1) return {nums[j], j + 1};
    return {-1, -1};
}

int main() {
    // Problem 1
    vector<int> arr1 = {3,1,5,4,2};
    cyclicSort(arr1);
    for (int v : arr1) cout << v << " "; cout << "\n"; // 1 2 3 4 5

    // Problem 2
    cout << missingNumber({3,0,1}) << "\n"; // 2

    // Problem 3
    auto p3 = findDisappearedNumbers({4,3,2,7,8,2,3,1});
    for (int v : p3) cout << v << " "; cout << "\n"; // 5 6

    // Problem 4
    cout << findDuplicate({1,3,4,2,2}) << "\n"; // 2

    // Problem 5
    auto p5 = findAllDuplicates({4,3,2,7,8,2,3,1});
    for (int v : p5) cout << v << " "; cout << "\n"; // 2 3 (order may vary)

    // Problem 6
    cout << firstMissingPositive({3,4,-1,1}) << "\n"; // 2

    // Problem 7
    auto p7 = findCorruptPair({3,1,2,5,2});
    cout << p7.first << " " << p7.second << "\n"; // 2 4

    // Problem 8
    auto p8 = kMissingPositive({2,4,1,5}, 2);
    for (int v : p8) cout << v << " "; cout << "\n"; // 3 6

    // Problem 9
    auto p9 = findErrorNums({1,2,2,4});
    cout << p9[0] << " " << p9[1] << "\n"; // 2 3

    return 0;
}
