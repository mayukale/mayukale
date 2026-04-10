/*
 * PATTERN: Backtracking
 *
 * CONCEPT:
 * Explore all potential candidates incrementally. At each step either accept
 * a candidate (recurse deeper) or reject it (prune and backtrack). Core idea:
 * "try → recurse → undo". Pruning (early termination of invalid paths) is
 * what separates backtracking from brute force.
 *
 * TIME:  Exponential in general (O(n!), O(2^n)); pruning reduces constant
 * SPACE: O(depth) recursion stack + O(result)
 *
 * WHEN TO USE:
 * - Generate all combinations / permutations / subsets
 * - Constraint satisfaction: Sudoku, N-Queens, graph coloring
 * - Path finding: word search, maze, knight's tour
 * - "Find all" or "count all valid arrangements"
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Combination Sum  [Difficulty: Medium]
// Source: LeetCode 39
// ─────────────────────────────────────────────
// Approach: Sorted candidates; pick same element repeatedly; prune when > target.
// Time: O(n^(t/min))  Space: O(t/min)
void combSumHelper(const vector<int>& c, int target, int start,
                   vector<int>& path, vector<vector<int>>& res) {
    if (target == 0) { res.push_back(path); return; }
    for (int i = start; i < (int)c.size() && c[i] <= target; ++i) {
        path.push_back(c[i]);
        combSumHelper(c, target - c[i], i, path, res);
        path.pop_back();
    }
}
vector<vector<int>> combinationSum(vector<int> c, int target) {
    sort(c.begin(), c.end());
    vector<vector<int>> res; vector<int> path;
    combSumHelper(c, target, 0, path, res);
    return res;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Sudoku Solver  [Difficulty: Hard]
// Source: LeetCode 37
// ─────────────────────────────────────────────
// Approach: Try digits 1-9 in each empty cell; validate row/col/box constraints.
// Time: O(9^(empty cells))  Space: O(81)
bool isValidSudoku(vector<vector<char>>& board, int r, int c, char d) {
    for (int i = 0; i < 9; ++i) {
        if (board[r][i] == d || board[i][c] == d) return false;
        if (board[3*(r/3)+i/3][3*(c/3)+i%3] == d) return false;
    }
    return true;
}
bool solveSudoku(vector<vector<char>>& board) {
    for (int r = 0; r < 9; ++r) for (int c = 0; c < 9; ++c) {
        if (board[r][c] == '.') {
            for (char d = '1'; d <= '9'; ++d) {
                if (isValidSudoku(board, r, c, d)) {
                    board[r][c] = d;
                    if (solveSudoku(board)) return true;
                    board[r][c] = '.';
                }
            }
            return false; // no digit worked → backtrack
        }
    }
    return true;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Word Search  [Difficulty: Medium]
// Source: LeetCode 79
// ─────────────────────────────────────────────
// Approach: DFS on each starting cell; mark visited by temp-swapping character.
// Time: O(m*n*4^L)  Space: O(L)
bool wordSearchDFS(vector<vector<char>>& board, const string& word,
                   int r, int c, int idx) {
    if (idx == (int)word.size()) return true;
    int m = (int)board.size(), n = (int)board[0].size();
    if (r<0||r>=m||c<0||c>=n||board[r][c]!=word[idx]) return false;
    char tmp = board[r][c]; board[r][c] = '#';
    bool found = wordSearchDFS(board, word, r+1,c, idx+1) ||
                 wordSearchDFS(board, word, r-1,c, idx+1) ||
                 wordSearchDFS(board, word, r,c+1, idx+1) ||
                 wordSearchDFS(board, word, r,c-1, idx+1);
    board[r][c] = tmp;
    return found;
}
bool exist(vector<vector<char>>& board, const string& word) {
    for (int r = 0; r < (int)board.size(); ++r)
        for (int c = 0; c < (int)board[0].size(); ++c)
            if (wordSearchDFS(board, word, r, c, 0)) return true;
    return false;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Permutations  [Difficulty: Medium]
// Source: LeetCode 46
// ─────────────────────────────────────────────
// Approach: Swap in-place: swap nums[i] into position `start`, recurse, swap back.
// Time: O(n*n!)  Space: O(n)
void permuteHelper(vector<int>& nums, int start, vector<vector<int>>& res) {
    if (start == (int)nums.size()) { res.push_back(nums); return; }
    for (int i = start; i < (int)nums.size(); ++i) {
        swap(nums[start], nums[i]);
        permuteHelper(nums, start + 1, res);
        swap(nums[start], nums[i]);
    }
}
vector<vector<int>> permute(vector<int> nums) {
    vector<vector<int>> res;
    permuteHelper(nums, 0, res);
    return res;
}

// ─────────────────────────────────────────────
// PROBLEM 5: N-Queens  [Difficulty: Hard]
// Source: LeetCode 51
// ─────────────────────────────────────────────
// Approach: Place one queen per row; O(1) check with column/diagonal sets.
// Time: O(n!)  Space: O(n²)
vector<vector<string>> solveNQueens(int n) {
    vector<vector<string>> res;
    vector<string> board(n, string(n, '.'));
    unordered_set<int> cols, diag, antiDiag;
    function<void(int)> bt = [&](int row) {
        if (row == n) { res.push_back(board); return; }
        for (int col = 0; col < n; ++col) {
            if (cols.count(col)||diag.count(row-col)||antiDiag.count(row+col)) continue;
            board[row][col] = 'Q';
            cols.insert(col); diag.insert(row-col); antiDiag.insert(row+col);
            bt(row+1);
            board[row][col] = '.';
            cols.erase(col); diag.erase(row-col); antiDiag.erase(row+col);
        }
    };
    bt(0);
    return res;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Restore IP Addresses  [Difficulty: Medium]
// Source: LeetCode 93
// ─────────────────────────────────────────────
// Approach: Backtracking; 4 parts, each 0-255, no leading zeros.
// Time: O(1) — at most 3^4 = 81 possibilities  Space: O(1)
vector<string> restoreIpAddresses(const string& s) {
    vector<string> result;
    vector<string> parts;
    function<void(int)> bt = [&](int start) {
        if ((int)parts.size() == 4 && start == (int)s.size()) {
            result.push_back(parts[0]+"."+parts[1]+"."+parts[2]+"."+parts[3]);
            return;
        }
        if ((int)parts.size() == 4 || start == (int)s.size()) return;
        for (int len = 1; len <= 3 && start + len <= (int)s.size(); ++len) {
            string seg = s.substr(start, len);
            if (seg.size() > 1 && seg[0] == '0') break;
            if (stoi(seg) > 255) break;
            parts.push_back(seg);
            bt(start + len);
            parts.pop_back();
        }
    };
    bt(0);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Letter Case Permutation  [Difficulty: Medium]
// Source: LeetCode 784
// ─────────────────────────────────────────────
// Approach: For each letter, branch into lowercase and uppercase.
// Time: O(2^L * n)  Space: O(2^L * n)
vector<string> letterCasePermutation(string s) {
    vector<string> result;
    function<void(int)> bt = [&](int i) {
        if (i == (int)s.size()) { result.push_back(s); return; }
        bt(i + 1);
        if (isalpha(s[i])) {
            s[i] ^= 32; // toggle case
            bt(i + 1);
            s[i] ^= 32;
        }
    };
    bt(0);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Rat in a Maze  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: DFS from (0,0) to (n-1,n-1); mark visited; collect valid paths.
// Time: O(4^(n²))  Space: O(n²)
vector<string> findPaths(vector<vector<int>>& maze) {
    int n = (int)maze.size();
    vector<string> result; string path;
    const string dirs = "DLRU";
    const int dr[] = {1,0,0,-1}, dc[] = {0,-1,1,0};
    function<void(int,int)> dfs = [&](int r, int c) {
        if (r == n-1 && c == n-1) { result.push_back(path); return; }
        maze[r][c] = 0;
        for (int d = 0; d < 4; ++d) {
            int nr = r+dr[d], nc = c+dc[d];
            if (nr>=0&&nr<n&&nc>=0&&nc<n&&maze[nr][nc]==1) {
                path += dirs[d]; dfs(nr,nc); path.pop_back();
            }
        }
        maze[r][c] = 1;
    };
    if (maze[0][0]) dfs(0, 0);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Count All Paths in Matrix  [Difficulty: Medium]
// Source: Classic DP / backtracking
// ─────────────────────────────────────────────
// Approach: DFS counting paths from top-left to bottom-right (right/down only).
// Time: O(2^(m+n))  Space: O(m+n)
int countPaths(int m, int n, int r = 0, int c = 0) {
    if (r == m-1 && c == n-1) return 1;
    int total = 0;
    if (r+1 < m) total += countPaths(m, n, r+1, c);
    if (c+1 < n) total += countPaths(m, n, r, c+1);
    return total;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Palindrome Partitioning  [Difficulty: Medium]
// Source: LeetCode 131
// ─────────────────────────────────────────────
// Approach: Backtracking + precomputed isPalin table.
// Time: O(n * 2^n)  Space: O(n²)
vector<vector<string>> partition(const string& s) {
    int n = (int)s.size();
    // Precompute palindrome table
    vector<vector<bool>> isPalin(n, vector<bool>(n, false));
    for (int i = n-1; i >= 0; --i)
        for (int j = i; j < n; ++j)
            isPalin[i][j] = (s[i]==s[j]) && (j-i<=2 || isPalin[i+1][j-1]);
    vector<vector<string>> result; vector<string> cur;
    function<void(int)> bt = [&](int start) {
        if (start == n) { result.push_back(cur); return; }
        for (int end = start; end < n; ++end) {
            if (isPalin[start][end]) {
                cur.push_back(s.substr(start, end-start+1));
                bt(end+1); cur.pop_back();
            }
        }
    };
    bt(0);
    return result;
}

int main() {
    // Problem 1
    auto p1 = combinationSum({2,3,6,7}, 7);
    for (auto& v : p1) { for (int x : v) cout << x << " "; cout << "\n"; }

    // Problem 2
    vector<vector<char>> board = {
        {'5','3','.','.','7','.','.','.','.'},
        {'6','.','.','1','9','5','.','.','.'},
        {'.','9','8','.','.','.','.','6','.'},
        {'8','.','.','.','6','.','.','.','3'},
        {'4','.','.','8','.','3','.','.','1'},
        {'7','.','.','.','2','.','.','.','6'},
        {'.','6','.','.','.','.','2','8','.'},
        {'.','.','.','4','1','9','.','.','5'},
        {'.','.','.','.','8','.','.','7','9'}
    };
    solveSudoku(board);
    cout << board[0][2] << "\n"; // 4

    // Problem 3
    vector<vector<char>> b3 = {{'A','B','C','E'},{'S','F','C','S'},{'A','D','E','E'}};
    cout << boolalpha << exist(b3, "ABCCED") << "\n"; // true

    // Problem 4
    auto p4 = permute({1,2,3});
    cout << p4.size() << " permutations\n"; // 6

    // Problem 5
    auto p5 = solveNQueens(4);
    cout << p5.size() << " solutions\n"; // 2

    // Problem 6
    auto p6 = restoreIpAddresses("25525511135");
    for (auto& s : p6) cout << s << " "; cout << "\n";

    // Problem 7
    auto p7 = letterCasePermutation("a1b2");
    cout << p7.size() << " results\n"; // 4

    // Problem 8
    vector<vector<int>> maze = {{1,0,0,0},{1,1,0,1},{1,1,0,0},{0,1,1,1}};
    auto p8 = findPaths(maze);
    for (auto& s : p8) cout << s << " "; cout << "\n";

    // Problem 9
    cout << countPaths(3, 3) << "\n"; // 6

    // Problem 10
    auto p10 = partition("aab");
    for (auto& v : p10) { for (auto& s : v) cout << s << " "; cout << "\n"; }

    return 0;
}
