/*
 * PATTERN: Sliding Window
 *
 * CONCEPT:
 * Maintain a contiguous window of elements in an array/string and slide it
 * forward while tracking some aggregate (sum, count, set of chars, etc.).
 * Avoids recomputing from scratch on each step: O(n) instead of O(n*k).
 *
 * TIME:  O(n)  — each element enters and leaves the window at most once
 * SPACE: O(k)  — k = window size or alphabet size for auxiliary structures
 *
 * WHEN TO USE:
 * - "Contiguous subarray / substring" with a size or value constraint
 * - Maximize/minimize length of a window satisfying a condition
 * - "At most K distinct", "longest without repeating", sum == target
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Maximum Sum Subarray of Size K  [Difficulty: Easy]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Fixed-size window; slide by adding right element, removing left.
// Time: O(n)  Space: O(1)
int maxSumSubarrayK(const vector<int>& nums, int k) {
    int windowSum = 0, maxSum = 0;
    for (int i = 0; i < (int)nums.size(); ++i) {
        windowSum += nums[i];
        if (i >= k - 1) {
            maxSum = max(maxSum, windowSum);
            windowSum -= nums[i - k + 1];
        }
    }
    return maxSum;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Smallest Subarray with Sum >= S  [Difficulty: Easy]
// Source: LeetCode 209 (Minimum Size Subarray Sum)
// ─────────────────────────────────────────────
// Approach: Variable window — shrink from left when sum >= s.
// Time: O(n)  Space: O(1)
int minSubarrayLen(int s, const vector<int>& nums) {
    int left = 0, sum = 0, minLen = INT_MAX;
    for (int right = 0; right < (int)nums.size(); ++right) {
        sum += nums[right];
        while (sum >= s) {
            minLen = min(minLen, right - left + 1);
            sum -= nums[left++];
        }
    }
    return minLen == INT_MAX ? 0 : minLen;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Longest Substring Without Repeating Characters  [Difficulty: Medium]
// Source: LeetCode 3
// ─────────────────────────────────────────────
// Approach: HashMap of last-seen index; jump left pointer past the duplicate.
// Time: O(n)  Space: O(min(n, 128))
int lengthOfLongestSubstring(const string& s) {
    unordered_map<char, int> lastSeen;
    int left = 0, maxLen = 0;
    for (int right = 0; right < (int)s.size(); ++right) {
        if (lastSeen.count(s[right]) && lastSeen[s[right]] >= left)
            left = lastSeen[s[right]] + 1;
        lastSeen[s[right]] = right;
        maxLen = max(maxLen, right - left + 1);
    }
    return maxLen;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Longest Substring with K Distinct Characters  [Difficulty: Medium]
// Source: LeetCode 340
// ─────────────────────────────────────────────
// Approach: Frequency map; shrink window when distinct chars > k.
// Time: O(n)  Space: O(k)
int lengthOfLongestSubstringKDistinct(const string& s, int k) {
    unordered_map<char, int> freq;
    int left = 0, maxLen = 0;
    for (int right = 0; right < (int)s.size(); ++right) {
        ++freq[s[right]];
        while ((int)freq.size() > k) {
            if (--freq[s[left]] == 0) freq.erase(s[left]);
            ++left;
        }
        maxLen = max(maxLen, right - left + 1);
    }
    return maxLen;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Fruits Into Baskets  [Difficulty: Medium]
// Source: LeetCode 904
// ─────────────────────────────────────────────
// Approach: At-most-2-distinct sliding window.
// Time: O(n)  Space: O(1)
int totalFruit(const vector<int>& fruits) {
    unordered_map<int, int> basket;
    int left = 0, maxLen = 0;
    for (int right = 0; right < (int)fruits.size(); ++right) {
        ++basket[fruits[right]];
        while ((int)basket.size() > 2) {
            if (--basket[fruits[left]] == 0) basket.erase(fruits[left]);
            ++left;
        }
        maxLen = max(maxLen, right - left + 1);
    }
    return maxLen;
}

// ─────────────────────────────────────────────
// PROBLEM 6: No-repeat Replacement — Longest Repeating Character Replacement  [Difficulty: Medium]
// Source: LeetCode 424
// ─────────────────────────────────────────────
// Approach: Track max freq in window; if (window - maxFreq) > k, slide left.
// Time: O(n)  Space: O(26)
int characterReplacement(const string& s, int k) {
    vector<int> freq(26, 0);
    int left = 0, maxFreq = 0, maxLen = 0;
    for (int right = 0; right < (int)s.size(); ++right) {
        maxFreq = max(maxFreq, ++freq[s[right] - 'A']);
        if ((right - left + 1) - maxFreq > k) {
            --freq[s[left++] - 'A'];
        }
        maxLen = max(maxLen, right - left + 1);
    }
    return maxLen;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Permutation in String  [Difficulty: Medium]
// Source: LeetCode 567
// ─────────────────────────────────────────────
// Approach: Fixed window of size |s1|; compare frequency arrays.
// Time: O(n)  Space: O(26)
bool checkInclusion(const string& s1, const string& s2) {
    if (s1.size() > s2.size()) return false;
    vector<int> need(26, 0), have(26, 0);
    for (char c : s1) ++need[c - 'a'];
    int k = (int)s1.size();
    for (int i = 0; i < (int)s2.size(); ++i) {
        ++have[s2[i] - 'a'];
        if (i >= k) --have[s2[i - k] - 'a'];
        if (have == need) return true;
    }
    return false;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Minimum Window Substring  [Difficulty: Hard]
// Source: LeetCode 76
// ─────────────────────────────────────────────
// Approach: Expand right to satisfy all chars; shrink left to minimize window.
// Time: O(n)  Space: O(|charset|)
string minWindow(const string& s, const string& t) {
    unordered_map<char, int> need;
    for (char c : t) ++need[c];
    int have = 0, required = (int)need.size();
    int left = 0, minLen = INT_MAX, startIdx = 0;
    unordered_map<char, int> window;
    for (int right = 0; right < (int)s.size(); ++right) {
        char c = s[right];
        ++window[c];
        if (need.count(c) && window[c] == need[c]) ++have;
        while (have == required) {
            if (right - left + 1 < minLen) {
                minLen = right - left + 1;
                startIdx = left;
            }
            char lc = s[left++];
            if (need.count(lc) && --window[lc] < need[lc]) --have;
        }
    }
    return minLen == INT_MAX ? "" : s.substr(startIdx, minLen);
}

// ─────────────────────────────────────────────
// PROBLEM 9: Sliding Window Maximum  [Difficulty: Hard]
// Source: LeetCode 239
// ─────────────────────────────────────────────
// Approach: Monotonic deque stores indices in decreasing order of value.
// Time: O(n)  Space: O(k)
vector<int> maxSlidingWindow(const vector<int>& nums, int k) {
    deque<int> dq; // indices
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
// PROBLEM 10: Substrings with At Most K Distinct  [Difficulty: Medium]
// Source: LeetCode 992 (variant — count subarrays with exactly k)
// ─────────────────────────────────────────────
// Approach: atMost(k) - atMost(k-1) trick.
// Time: O(n)  Space: O(k)
int subarraysWithAtMostKDistinct(const vector<int>& A, int k) {
    unordered_map<int, int> freq;
    int left = 0, count = 0;
    for (int right = 0; right < (int)A.size(); ++right) {
        ++freq[A[right]];
        while ((int)freq.size() > k) {
            if (--freq[A[left]] == 0) freq.erase(A[left]);
            ++left;
        }
        count += right - left + 1;
    }
    return count;
}
int subarraysWithExactlyKDistinct(const vector<int>& A, int k) {
    return subarraysWithAtMostKDistinct(A, k) - subarraysWithAtMostKDistinct(A, k - 1);
}

// ─────────────────────────────────────────────
// PROBLEM 11: Max Consecutive Ones III  [Difficulty: Medium]
// Source: LeetCode 1004
// ─────────────────────────────────────────────
// Approach: Window with at most k zeros; shrink when zeros > k.
// Time: O(n)  Space: O(1)
int longestOnes(const vector<int>& nums, int k) {
    int left = 0, zeros = 0, maxLen = 0;
    for (int right = 0; right < (int)nums.size(); ++right) {
        if (nums[right] == 0) ++zeros;
        while (zeros > k) {
            if (nums[left++] == 0) --zeros;
        }
        maxLen = max(maxLen, right - left + 1);
    }
    return maxLen;
}

int main() {
    // Problem 1
    cout << maxSumSubarrayK({2,1,5,1,3,2}, 3) << "\n"; // 9

    // Problem 2
    cout << minSubarrayLen(7, {2,3,1,2,4,3}) << "\n"; // 2

    // Problem 3
    cout << lengthOfLongestSubstring("abcabcbb") << "\n"; // 3

    // Problem 4
    cout << lengthOfLongestSubstringKDistinct("araaci", 2) << "\n"; // 4

    // Problem 5
    cout << totalFruit({1,2,1,2,3}) << "\n"; // 4

    // Problem 6
    cout << characterReplacement("AABABBA", 1) << "\n"; // 4

    // Problem 7
    cout << boolalpha << checkInclusion("ab", "eidbaooo") << "\n"; // true

    // Problem 8
    cout << minWindow("ADOBECODEBANC", "ABC") << "\n"; // "BANC"

    // Problem 9
    auto sw = maxSlidingWindow({1,3,-1,-3,5,3,6,7}, 3);
    for (int v : sw) cout << v << " "; cout << "\n"; // 3 3 5 5 6 7

    // Problem 10
    cout << subarraysWithExactlyKDistinct({1,2,1,2,3}, 2) << "\n"; // 7

    // Problem 11
    cout << longestOnes({0,0,1,1,0,0,1,1,1,0,1,1,0,0,0,1,1,1,1}, 3) << "\n"; // 10

    return 0;
}
