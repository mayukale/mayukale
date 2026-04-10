/*
 * PATTERN: Subsets (BFS / Cascading / Backtracking)
 *
 * CONCEPT:
 * Generate all possible subsets or permutations by starting with an empty set
 * and iteratively adding each new element to all existing subsets (BFS/cascading).
 * Alternatively, use backtracking with a running path. For problems with
 * duplicates, sort first and skip consecutive equal elements.
 *
 * TIME:  O(n * 2^n) for subsets, O(n * n!) for permutations
 * SPACE: O(n * 2^n) or O(n * n!)
 *
 * WHEN TO USE:
 * - "Generate all subsets / power set"
 * - "Combination sum", "permutations", "letter combinations"
 * - Problems requiring exhaustive enumeration of choices
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Subsets (no duplicates)  [Difficulty: Medium]
// Source: LeetCode 78
// ─────────────────────────────────────────────
// Approach: Cascading — for each element, clone all existing subsets and add element.
// Time: O(n*2^n)  Space: O(n*2^n)
vector<vector<int>> subsets(const vector<int>& nums) {
    vector<vector<int>> result = {{}};
    for (int num : nums) {
        int sz = (int)result.size();
        for (int i = 0; i < sz; ++i) {
            result.push_back(result[i]);
            result.back().push_back(num);
        }
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Subsets II (with duplicates)  [Difficulty: Medium]
// Source: LeetCode 90
// ─────────────────────────────────────────────
// Approach: Sort; cascading but new element only extends subsets added in last round.
// Time: O(n*2^n)  Space: O(n*2^n)
vector<vector<int>> subsetsWithDup(vector<int> nums) {
    sort(nums.begin(), nums.end());
    vector<vector<int>> result = {{}};
    int start = 0, end = 0;
    for (int i = 0; i < (int)nums.size(); ++i) {
        start = (i > 0 && nums[i] == nums[i-1]) ? end : 0;
        end = (int)result.size();
        for (int j = start; j < end; ++j) {
            result.push_back(result[j]);
            result.back().push_back(nums[i]);
        }
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Permutations (no duplicates)  [Difficulty: Medium]
// Source: LeetCode 46
// ─────────────────────────────────────────────
// Approach: Backtracking with a used[] mask.
// Time: O(n*n!)  Space: O(n*n!)
void permuteHelper(const vector<int>& nums, vector<bool>& used,
                   vector<int>& cur, vector<vector<int>>& result) {
    if ((int)cur.size() == (int)nums.size()) { result.push_back(cur); return; }
    for (int i = 0; i < (int)nums.size(); ++i) {
        if (used[i]) continue;
        used[i] = true; cur.push_back(nums[i]);
        permuteHelper(nums, used, cur, result);
        used[i] = false; cur.pop_back();
    }
}
vector<vector<int>> permute(const vector<int>& nums) {
    vector<vector<int>> result; vector<int> cur; vector<bool> used(nums.size(), false);
    permuteHelper(nums, used, cur, result);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Permutations II (with duplicates)  [Difficulty: Medium]
// Source: LeetCode 47
// ─────────────────────────────────────────────
// Approach: Sort first; skip used[i] or (nums[i]==nums[i-1] && !used[i-1]).
// Time: O(n*n!)  Space: O(n*n!)
void permuteUniqueHelper(vector<int>& nums, vector<bool>& used,
                         vector<int>& cur, vector<vector<int>>& result) {
    if ((int)cur.size() == (int)nums.size()) { result.push_back(cur); return; }
    for (int i = 0; i < (int)nums.size(); ++i) {
        if (used[i]) continue;
        if (i > 0 && nums[i] == nums[i-1] && !used[i-1]) continue;
        used[i] = true; cur.push_back(nums[i]);
        permuteUniqueHelper(nums, used, cur, result);
        used[i] = false; cur.pop_back();
    }
}
vector<vector<int>> permuteUnique(vector<int> nums) {
    sort(nums.begin(), nums.end());
    vector<vector<int>> result; vector<int> cur; vector<bool> used(nums.size(), false);
    permuteUniqueHelper(nums, used, cur, result);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Combination Sum  [Difficulty: Medium]
// Source: LeetCode 39
// ─────────────────────────────────────────────
// Approach: Backtracking; reuse same element allowed; prune when sum > target.
// Time: O(n^(t/m)) where t=target, m=min element  Space: O(t/m)
void combinationSumHelper(const vector<int>& candidates, int target,
                          int start, vector<int>& cur, vector<vector<int>>& result) {
    if (target == 0) { result.push_back(cur); return; }
    for (int i = start; i < (int)candidates.size(); ++i) {
        if (candidates[i] > target) break;
        cur.push_back(candidates[i]);
        combinationSumHelper(candidates, target - candidates[i], i, cur, result);
        cur.pop_back();
    }
}
vector<vector<int>> combinationSum(vector<int> candidates, int target) {
    sort(candidates.begin(), candidates.end());
    vector<vector<int>> result; vector<int> cur;
    combinationSumHelper(candidates, target, 0, cur, result);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Combination Sum II (each element used once)  [Difficulty: Medium]
// Source: LeetCode 40
// ─────────────────────────────────────────────
// Approach: Sort; skip duplicate siblings (same value at same depth).
// Time: O(2^n)  Space: O(n)
void combinationSum2Helper(const vector<int>& c, int target,
                           int start, vector<int>& cur, vector<vector<int>>& result) {
    if (target == 0) { result.push_back(cur); return; }
    for (int i = start; i < (int)c.size(); ++i) {
        if (c[i] > target) break;
        if (i > start && c[i] == c[i-1]) continue;
        cur.push_back(c[i]);
        combinationSum2Helper(c, target - c[i], i + 1, cur, result);
        cur.pop_back();
    }
}
vector<vector<int>> combinationSum2(vector<int> c, int target) {
    sort(c.begin(), c.end());
    vector<vector<int>> result; vector<int> cur;
    combinationSum2Helper(c, target, 0, cur, result);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Letter Combinations of a Phone Number  [Difficulty: Medium]
// Source: LeetCode 17
// ─────────────────────────────────────────────
// Approach: Backtracking with phone map; build string character by character.
// Time: O(4^n)  Space: O(n)
vector<string> letterCombinations(const string& digits) {
    if (digits.empty()) return {};
    const vector<string> phone = {"","","abc","def","ghi","jkl","mno","pqrs","tuv","wxyz"};
    vector<string> result; string cur;
    function<void(int)> bt = [&](int i) {
        if (i == (int)digits.size()) { result.push_back(cur); return; }
        for (char c : phone[digits[i] - '0']) {
            cur.push_back(c); bt(i + 1); cur.pop_back();
        }
    };
    bt(0);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Generate Parentheses  [Difficulty: Medium]
// Source: LeetCode 22
// ─────────────────────────────────────────────
// Approach: Backtracking; add '(' if open<n, add ')' if close<open.
// Time: O(4^n/sqrt(n)) Catalan  Space: O(n)
vector<string> generateParenthesis(int n) {
    vector<string> result; string cur;
    function<void(int,int)> bt = [&](int open, int close) {
        if ((int)cur.size() == 2 * n) { result.push_back(cur); return; }
        if (open < n)       { cur += '('; bt(open+1, close); cur.pop_back(); }
        if (close < open)   { cur += ')'; bt(open, close+1); cur.pop_back(); }
    };
    bt(0, 0);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Palindrome Partitioning  [Difficulty: Medium]
// Source: LeetCode 131
// ─────────────────────────────────────────────
// Approach: Backtracking; try all prefixes that are palindromes.
// Time: O(n*2^n)  Space: O(n^2)
bool isPalin(const string& s, int l, int r) {
    while (l < r) if (s[l++] != s[r--]) return false;
    return true;
}
vector<vector<string>> partition(const string& s) {
    vector<vector<string>> result; vector<string> cur;
    function<void(int)> bt = [&](int start) {
        if (start == (int)s.size()) { result.push_back(cur); return; }
        for (int end = start; end < (int)s.size(); ++end) {
            if (isPalin(s, start, end)) {
                cur.push_back(s.substr(start, end - start + 1));
                bt(end + 1);
                cur.pop_back();
            }
        }
    };
    bt(0);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 10: N-Queens  [Difficulty: Hard]
// Source: LeetCode 51
// ─────────────────────────────────────────────
// Approach: Place one queen per row; track column, diagonal, anti-diagonal sets.
// Time: O(n!)  Space: O(n²)
vector<vector<string>> solveNQueens(int n) {
    vector<vector<string>> result;
    vector<string> board(n, string(n, '.'));
    unordered_set<int> cols, diag, antiDiag;
    function<void(int)> bt = [&](int row) {
        if (row == n) { result.push_back(board); return; }
        for (int col = 0; col < n; ++col) {
            if (cols.count(col) || diag.count(row-col) || antiDiag.count(row+col)) continue;
            board[row][col] = 'Q';
            cols.insert(col); diag.insert(row-col); antiDiag.insert(row+col);
            bt(row + 1);
            board[row][col] = '.';
            cols.erase(col); diag.erase(row-col); antiDiag.erase(row+col);
        }
    };
    bt(0);
    return result;
}

int main() {
    // Problem 1
    auto p1 = subsets({1,2,3});
    cout << p1.size() << " subsets\n"; // 8

    // Problem 2
    auto p2 = subsetsWithDup({1,3,3});
    cout << p2.size() << " unique subsets\n"; // 6

    // Problem 3
    auto p3 = permute({1,2,3});
    cout << p3.size() << " permutations\n"; // 6

    // Problem 4
    auto p4 = permuteUnique({1,1,2});
    cout << p4.size() << " unique permutations\n"; // 3

    // Problem 5
    auto p5 = combinationSum({2,3,6,7}, 7);
    for (auto& v : p5) { for (int x : v) cout << x << " "; cout << "\n"; }

    // Problem 6
    auto p6 = combinationSum2({10,1,2,7,6,1,5}, 8);
    for (auto& v : p6) { for (int x : v) cout << x << " "; cout << "\n"; }

    // Problem 7
    auto p7 = letterCombinations("23");
    for (auto& s : p7) cout << s << " "; cout << "\n";

    // Problem 8
    auto p8 = generateParenthesis(3);
    for (auto& s : p8) cout << s << " "; cout << "\n";

    // Problem 9
    auto p9 = partition("aab");
    for (auto& v : p9) { for (auto& s : v) cout << s << " "; cout << "\n"; }

    // Problem 10
    auto p10 = solveNQueens(4);
    cout << p10.size() << " solutions\n"; // 2

    return 0;
}
