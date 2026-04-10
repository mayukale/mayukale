/*
 * PATTERN: Two Pointers
 *
 * CONCEPT:
 * Use two indices that move toward each other (or in the same direction) to
 * reduce an O(n²) brute-force to O(n). Works on sorted arrays or when a
 * second pointer can track a complementary element / slower progress.
 *
 * TIME:  O(n log n) if sort needed, O(n) scan after
 * SPACE: O(1) extra (or O(n) for result storage)
 *
 * WHEN TO USE:
 * - Find a pair/triplet/quadruplet summing to target in a sorted array
 * - Compare or process from both ends simultaneously
 * - In-place array manipulation (remove duplicates, move zeroes)
 * - "Container with most water" style problems
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Two Sum II (sorted input)  [Difficulty: Easy]
// Source: LeetCode 167
// ─────────────────────────────────────────────
// Approach: Left and right converge; adjust based on sum vs target.
// Time: O(n)  Space: O(1)
vector<int> twoSumSorted(const vector<int>& nums, int target) {
    int l = 0, r = (int)nums.size() - 1;
    while (l < r) {
        int s = nums[l] + nums[r];
        if (s == target) return {l + 1, r + 1};
        else if (s < target) ++l;
        else --r;
    }
    return {};
}

// ─────────────────────────────────────────────
// PROBLEM 2: Three Sum  [Difficulty: Medium]
// Source: LeetCode 15
// ─────────────────────────────────────────────
// Approach: Fix one element, two-pointer on the rest; skip duplicates.
// Time: O(n²)  Space: O(1) extra (output not counted)
vector<vector<int>> threeSum(vector<int> nums) {
    sort(nums.begin(), nums.end());
    vector<vector<int>> result;
    for (int i = 0; i < (int)nums.size() - 2; ++i) {
        if (i > 0 && nums[i] == nums[i - 1]) continue;
        int l = i + 1, r = (int)nums.size() - 1;
        while (l < r) {
            int s = nums[i] + nums[l] + nums[r];
            if (s == 0) {
                result.push_back({nums[i], nums[l], nums[r]});
                while (l < r && nums[l] == nums[l + 1]) ++l;
                while (l < r && nums[r] == nums[r - 1]) --r;
                ++l; --r;
            } else if (s < 0) ++l;
            else --r;
        }
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Three Sum Closest  [Difficulty: Medium]
// Source: LeetCode 16
// ─────────────────────────────────────────────
// Approach: Sort + two-pointer; track closest sum.
// Time: O(n²)  Space: O(1)
int threeSumClosest(vector<int> nums, int target) {
    sort(nums.begin(), nums.end());
    int closest = nums[0] + nums[1] + nums[2];
    for (int i = 0; i < (int)nums.size() - 2; ++i) {
        int l = i + 1, r = (int)nums.size() - 1;
        while (l < r) {
            int s = nums[i] + nums[l] + nums[r];
            if (abs(s - target) < abs(closest - target)) closest = s;
            if (s < target) ++l;
            else if (s > target) --r;
            else return s;
        }
    }
    return closest;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Four Sum  [Difficulty: Medium]
// Source: LeetCode 18
// ─────────────────────────────────────────────
// Approach: Two outer loops + two-pointer inner; skip duplicates at each level.
// Time: O(n³)  Space: O(1)
vector<vector<int>> fourSum(vector<int> nums, int target) {
    sort(nums.begin(), nums.end());
    vector<vector<int>> result;
    int n = (int)nums.size();
    for (int i = 0; i < n - 3; ++i) {
        if (i > 0 && nums[i] == nums[i - 1]) continue;
        for (int j = i + 1; j < n - 2; ++j) {
            if (j > i + 1 && nums[j] == nums[j - 1]) continue;
            int l = j + 1, r = n - 1;
            while (l < r) {
                long long s = (long long)nums[i] + nums[j] + nums[l] + nums[r];
                if (s == target) {
                    result.push_back({nums[i], nums[j], nums[l], nums[r]});
                    while (l < r && nums[l] == nums[l + 1]) ++l;
                    while (l < r && nums[r] == nums[r - 1]) --r;
                    ++l; --r;
                } else if (s < target) ++l;
                else --r;
            }
        }
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Remove Duplicates from Sorted Array  [Difficulty: Easy]
// Source: LeetCode 26
// ─────────────────────────────────────────────
// Approach: Slow pointer tracks write position; fast scans for new values.
// Time: O(n)  Space: O(1)
int removeDuplicates(vector<int>& nums) {
    if (nums.empty()) return 0;
    int slow = 0;
    for (int fast = 1; fast < (int)nums.size(); ++fast)
        if (nums[fast] != nums[slow]) nums[++slow] = nums[fast];
    return slow + 1;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Container With Most Water  [Difficulty: Medium]
// Source: LeetCode 11
// ─────────────────────────────────────────────
// Approach: Greedy: always move the shorter side inward.
// Time: O(n)  Space: O(1)
int maxArea(const vector<int>& height) {
    int l = 0, r = (int)height.size() - 1, best = 0;
    while (l < r) {
        best = max(best, min(height[l], height[r]) * (r - l));
        if (height[l] < height[r]) ++l; else --r;
    }
    return best;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Trapping Rain Water  [Difficulty: Hard]
// Source: LeetCode 42
// ─────────────────────────────────────────────
// Approach: Two pointers track left/right max; water = min(lmax,rmax) - h[i].
// Time: O(n)  Space: O(1)
int trap(const vector<int>& height) {
    int l = 0, r = (int)height.size() - 1;
    int lMax = 0, rMax = 0, water = 0;
    while (l < r) {
        if (height[l] < height[r]) {
            lMax = max(lMax, height[l]);
            water += lMax - height[l++];
        } else {
            rMax = max(rMax, height[r]);
            water += rMax - height[r--];
        }
    }
    return water;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Squares of a Sorted Array  [Difficulty: Easy]
// Source: LeetCode 977
// ─────────────────────────────────────────────
// Approach: Two pointers from ends; largest square fills result from back.
// Time: O(n)  Space: O(n)
vector<int> sortedSquares(const vector<int>& nums) {
    int n = (int)nums.size();
    vector<int> result(n);
    int l = 0, r = n - 1, pos = n - 1;
    while (l <= r) {
        int lSq = nums[l] * nums[l], rSq = nums[r] * nums[r];
        if (lSq > rSq) { result[pos--] = lSq; ++l; }
        else { result[pos--] = rSq; --r; }
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Move Zeroes  [Difficulty: Easy]
// Source: LeetCode 283
// ─────────────────────────────────────────────
// Approach: Slow pointer = insert position for non-zeros; then fill rest with 0.
// Time: O(n)  Space: O(1)
void moveZeroes(vector<int>& nums) {
    int slow = 0;
    for (int fast = 0; fast < (int)nums.size(); ++fast)
        if (nums[fast] != 0) nums[slow++] = nums[fast];
    while (slow < (int)nums.size()) nums[slow++] = 0;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Valid Palindrome  [Difficulty: Easy]
// Source: LeetCode 125
// ─────────────────────────────────────────────
// Approach: Two pointers skip non-alphanumeric, compare case-insensitively.
// Time: O(n)  Space: O(1)
bool isPalindrome(const string& s) {
    int l = 0, r = (int)s.size() - 1;
    while (l < r) {
        while (l < r && !isalnum(s[l])) ++l;
        while (l < r && !isalnum(s[r])) --r;
        if (tolower(s[l++]) != tolower(s[r--])) return false;
    }
    return true;
}

// ─────────────────────────────────────────────
// PROBLEM 11: Boat to Save People  [Difficulty: Medium]
// Source: LeetCode 881
// ─────────────────────────────────────────────
// Approach: Sort; greedily pair heaviest with lightest if possible.
// Time: O(n log n)  Space: O(1)
int numRescueBoats(vector<int> people, int limit) {
    sort(people.begin(), people.end());
    int l = 0, r = (int)people.size() - 1, boats = 0;
    while (l <= r) {
        if (people[l] + people[r] <= limit) ++l;
        --r; ++boats;
    }
    return boats;
}

int main() {
    // Problem 1
    auto p1 = twoSumSorted({2,7,11,15}, 9);
    cout << p1[0] << " " << p1[1] << "\n"; // 1 2

    // Problem 2
    auto p2 = threeSum({-1,0,1,2,-1,-4});
    for (auto& v : p2) { for (int x : v) cout << x << " "; cout << "\n"; }

    // Problem 3
    cout << threeSumClosest({-1,2,1,-4}, 1) << "\n"; // 2

    // Problem 4
    auto p4 = fourSum({1,0,-1,0,-2,2}, 0);
    for (auto& v : p4) { for (int x : v) cout << x << " "; cout << "\n"; }

    // Problem 5
    vector<int> arr5 = {0,0,1,1,1,2,2,3,3,4};
    cout << removeDuplicates(arr5) << "\n"; // 5

    // Problem 6
    cout << maxArea({1,8,6,2,5,4,8,3,7}) << "\n"; // 49

    // Problem 7
    cout << trap({0,1,0,2,1,0,1,3,2,1,2,1}) << "\n"; // 6

    // Problem 8
    auto p8 = sortedSquares({-4,-1,0,3,10});
    for (int v : p8) cout << v << " "; cout << "\n"; // 0 1 9 16 100

    // Problem 9
    vector<int> arr9 = {0,1,0,3,12};
    moveZeroes(arr9);
    for (int v : arr9) cout << v << " "; cout << "\n"; // 1 3 12 0 0

    // Problem 10
    cout << boolalpha << isPalindrome("A man, a plan, a canal: Panama") << "\n"; // true

    // Problem 11
    cout << numRescueBoats({3,2,2,1}, 3) << "\n"; // 3

    return 0;
}
