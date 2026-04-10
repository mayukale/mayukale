/*
 * PATTERN: Graph Traversal (BFS & DFS)
 *
 * CONCEPT:
 * BFS explores neighbors level by level using a queue — finds shortest path in
 * unweighted graphs. DFS goes as deep as possible using recursion or a stack —
 * good for connectivity, cycle detection, and component analysis.
 * Both run in O(V+E). Key: mark visited before/when enqueuing to avoid cycles.
 *
 * TIME:  O(V + E)
 * SPACE: O(V)
 *
 * WHEN TO USE:
 * - Number of islands, connected components
 * - Shortest path in unweighted/unit-cost graph (BFS)
 * - Cycle detection, bipartite check, clone graph
 * - Word ladder, maze, grid problems
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Number of Islands  [Difficulty: Medium]
// Source: LeetCode 200
// ─────────────────────────────────────────────
// Approach: DFS flood-fill; mark visited by changing '1' to '0'.
// Time: O(m*n)  Space: O(m*n) recursion
int numIslands(vector<vector<char>> grid) {
    int m = (int)grid.size(), n = (int)grid[0].size(), count = 0;
    function<void(int,int)> dfs = [&](int r, int c) {
        if (r < 0 || r >= m || c < 0 || c >= n || grid[r][c] != '1') return;
        grid[r][c] = '0';
        dfs(r+1,c); dfs(r-1,c); dfs(r,c+1); dfs(r,c-1);
    };
    for (int r = 0; r < m; ++r)
        for (int c = 0; c < n; ++c)
            if (grid[r][c] == '1') { dfs(r,c); ++count; }
    return count;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Shortest Path in Binary Matrix  [Difficulty: Medium]
// Source: LeetCode 1091
// ─────────────────────────────────────────────
// Approach: BFS from (0,0); 8-directional; count levels.
// Time: O(n²)  Space: O(n²)
int shortestPathBinaryMatrix(vector<vector<int>> grid) {
    int n = (int)grid.size();
    if (grid[0][0] || grid[n-1][n-1]) return -1;
    const int dirs[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    queue<pair<int,int>> q;
    q.push({0,0}); grid[0][0] = 1;
    int dist = 1;
    while (!q.empty()) {
        int sz = (int)q.size();
        for (int i = 0; i < sz; ++i) {
            auto [r, c] = q.front(); q.pop();
            if (r == n-1 && c == n-1) return dist;
            for (auto& d : dirs) {
                int nr = r + d[0], nc = c + d[1];
                if (nr >= 0 && nr < n && nc >= 0 && nc < n && !grid[nr][nc]) {
                    grid[nr][nc] = 1;
                    q.push({nr, nc});
                }
            }
        }
        ++dist;
    }
    return -1;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Clone Graph  [Difficulty: Medium]
// Source: LeetCode 133
// ─────────────────────────────────────────────
// Approach: BFS/DFS; map from original node to its clone.
// Time: O(V+E)  Space: O(V)
struct GraphNode {
    int val;
    vector<GraphNode*> neighbors;
    explicit GraphNode(int v) : val(v) {}
};
GraphNode* cloneGraph(GraphNode* node) {
    if (!node) return nullptr;
    unordered_map<GraphNode*, GraphNode*> cloneMap;
    queue<GraphNode*> q;
    q.push(node);
    cloneMap[node] = new GraphNode(node->val);
    while (!q.empty()) {
        auto* cur = q.front(); q.pop();
        for (auto* nb : cur->neighbors) {
            if (!cloneMap.count(nb)) {
                cloneMap[nb] = new GraphNode(nb->val);
                q.push(nb);
            }
            cloneMap[cur]->neighbors.push_back(cloneMap[nb]);
        }
    }
    return cloneMap[node];
}

// ─────────────────────────────────────────────
// PROBLEM 4: Word Ladder  [Difficulty: Hard]
// Source: LeetCode 127
// ─────────────────────────────────────────────
// Approach: BFS; change one character at a time; use wordSet for O(1) lookup.
// Time: O(M² * N) M=word length, N=number of words  Space: O(M*N)
int ladderLength(const string& beginWord, const string& endWord,
                 const vector<string>& wordList) {
    unordered_set<string> wordSet(wordList.begin(), wordList.end());
    if (!wordSet.count(endWord)) return 0;
    queue<string> q;
    q.push(beginWord);
    int level = 1;
    while (!q.empty()) {
        int sz = (int)q.size();
        for (int i = 0; i < sz; ++i) {
            string word = q.front(); q.pop();
            for (int j = 0; j < (int)word.size(); ++j) {
                char orig = word[j];
                for (char c = 'a'; c <= 'z'; ++c) {
                    word[j] = c;
                    if (word == endWord) return level + 1;
                    if (wordSet.count(word)) { wordSet.erase(word); q.push(word); }
                }
                word[j] = orig;
            }
        }
        ++level;
    }
    return 0;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Is Graph Bipartite?  [Difficulty: Medium]
// Source: LeetCode 785
// ─────────────────────────────────────────────
// Approach: BFS 2-coloring; if neighbor same color → not bipartite.
// Time: O(V+E)  Space: O(V)
bool isBipartite(const vector<vector<int>>& graph) {
    int n = (int)graph.size();
    vector<int> color(n, -1);
    for (int start = 0; start < n; ++start) {
        if (color[start] != -1) continue;
        queue<int> q;
        q.push(start); color[start] = 0;
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int v : graph[u]) {
                if (color[v] == -1) { color[v] = 1 - color[u]; q.push(v); }
                else if (color[v] == color[u]) return false;
            }
        }
    }
    return true;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Pacific Atlantic Water Flow  [Difficulty: Medium]
// Source: LeetCode 417
// ─────────────────────────────────────────────
// Approach: Reverse BFS from both oceans simultaneously; find intersection.
// Time: O(m*n)  Space: O(m*n)
vector<vector<int>> pacificAtlantic(const vector<vector<int>>& heights) {
    int m = (int)heights.size(), n = (int)heights[0].size();
    vector<vector<bool>> pac(m, vector<bool>(n, false));
    vector<vector<bool>> atl(m, vector<bool>(n, false));
    const int dx[] = {0,0,1,-1}, dy[] = {1,-1,0,0};
    auto bfs = [&](queue<pair<int,int>>& q, vector<vector<bool>>& vis) {
        while (!q.empty()) {
            auto [r, c] = q.front(); q.pop();
            for (int d = 0; d < 4; ++d) {
                int nr = r + dx[d], nc = c + dy[d];
                if (nr < 0 || nr >= m || nc < 0 || nc >= n ||
                    vis[nr][nc] || heights[nr][nc] < heights[r][c]) continue;
                vis[nr][nc] = true;
                q.push({nr, nc});
            }
        }
    };
    queue<pair<int,int>> pq, aq;
    for (int r = 0; r < m; ++r) { pac[r][0]=true; pq.push({r,0}); atl[r][n-1]=true; aq.push({r,n-1}); }
    for (int c = 0; c < n; ++c) { pac[0][c]=true; pq.push({0,c}); atl[m-1][c]=true; aq.push({m-1,c}); }
    bfs(pq, pac); bfs(aq, atl);
    vector<vector<int>> result;
    for (int r = 0; r < m; ++r) for (int c = 0; c < n; ++c) if (pac[r][c] && atl[r][c]) result.push_back({r,c});
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Rotting Oranges  [Difficulty: Medium]
// Source: LeetCode 994
// ─────────────────────────────────────────────
// Approach: Multi-source BFS from all rotten oranges simultaneously.
// Time: O(m*n)  Space: O(m*n)
int orangesRotting(vector<vector<int>> grid) {
    int m = (int)grid.size(), n = (int)grid[0].size();
    queue<pair<int,int>> q; int fresh = 0;
    for (int r = 0; r < m; ++r) for (int c = 0; c < n; ++c) {
        if (grid[r][c] == 2) q.push({r,c});
        else if (grid[r][c] == 1) ++fresh;
    }
    if (!fresh) return 0;
    const int dx[] = {0,0,1,-1}, dy[] = {1,-1,0,0};
    int minutes = 0;
    while (!q.empty()) {
        ++minutes;
        int sz = (int)q.size();
        for (int i = 0; i < sz; ++i) {
            auto [r, c] = q.front(); q.pop();
            for (int d = 0; d < 4; ++d) {
                int nr = r+dx[d], nc = c+dy[d];
                if (nr>=0 && nr<m && nc>=0 && nc<n && grid[nr][nc]==1) {
                    grid[nr][nc] = 2; --fresh; q.push({nr,nc});
                }
            }
        }
    }
    return fresh == 0 ? minutes - 1 : -1;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Number of Connected Components  [Difficulty: Medium]
// Source: LeetCode 323
// ─────────────────────────────────────────────
// Approach: DFS/BFS for each unvisited node; count components.
// Time: O(V+E)  Space: O(V)
int countComponents(int n, const vector<vector<int>>& edges) {
    vector<vector<int>> adj(n);
    for (auto& e : edges) { adj[e[0]].push_back(e[1]); adj[e[1]].push_back(e[0]); }
    vector<bool> visited(n, false);
    int components = 0;
    function<void(int)> dfs = [&](int u) {
        visited[u] = true;
        for (int v : adj[u]) if (!visited[v]) dfs(v);
    };
    for (int i = 0; i < n; ++i) if (!visited[i]) { dfs(i); ++components; }
    return components;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Walls and Gates  [Difficulty: Medium]
// Source: LeetCode 286
// ─────────────────────────────────────────────
// Approach: Multi-source BFS from all gates (0); fill INF cells with distance.
// Time: O(m*n)  Space: O(m*n)
void wallsAndGates(vector<vector<int>>& rooms) {
    const int INF = INT_MAX;
    int m = (int)rooms.size(), n = (int)rooms[0].size();
    queue<pair<int,int>> q;
    for (int r = 0; r < m; ++r) for (int c = 0; c < n; ++c) if (rooms[r][c] == 0) q.push({r,c});
    const int dx[] = {0,0,1,-1}, dy[] = {1,-1,0,0};
    while (!q.empty()) {
        auto [r, c] = q.front(); q.pop();
        for (int d = 0; d < 4; ++d) {
            int nr = r+dx[d], nc = c+dy[d];
            if (nr>=0&&nr<m&&nc>=0&&nc<n&&rooms[nr][nc]==INF) {
                rooms[nr][nc] = rooms[r][c] + 1;
                q.push({nr,nc});
            }
        }
    }
}

// ─────────────────────────────────────────────
// PROBLEM 10: Graph Valid Tree  [Difficulty: Medium]
// Source: LeetCode 261
// ─────────────────────────────────────────────
// Approach: DFS; valid tree iff connected + no cycle (edges == n-1 is quick check).
// Time: O(V+E)  Space: O(V)
bool validTree(int n, const vector<vector<int>>& edges) {
    if ((int)edges.size() != n - 1) return false;
    vector<vector<int>> adj(n);
    for (auto& e : edges) { adj[e[0]].push_back(e[1]); adj[e[1]].push_back(e[0]); }
    vector<bool> visited(n, false);
    function<void(int,int)> dfs = [&](int u, int parent) {
        visited[u] = true;
        for (int v : adj[u]) if (!visited[v]) dfs(v, u);
    };
    dfs(0, -1);
    for (bool v : visited) if (!v) return false;
    return true;
}

int main() {
    // Problem 1
    cout << numIslands({{'1','1','1','1','0'},
                        {'1','1','0','1','0'},
                        {'1','1','0','0','0'},
                        {'0','0','0','0','0'}}) << "\n"; // 1

    // Problem 2
    cout << shortestPathBinaryMatrix({{0,0,0},{1,1,0},{1,1,0}}) << "\n"; // 4

    // Problem 4
    cout << ladderLength("hit", "cog", {"hot","dot","dog","lot","log","cog"}) << "\n"; // 5

    // Problem 5
    cout << boolalpha << isBipartite({{1,3},{0,2},{1,3},{0,2}}) << "\n"; // true

    // Problem 7
    cout << orangesRotting({{2,1,1},{1,1,0},{0,1,1}}) << "\n"; // 4

    // Problem 8
    cout << countComponents(5, {{0,1},{1,2},{3,4}}) << "\n"; // 2

    // Problem 9
    const int INF = INT_MAX;
    vector<vector<int>> rooms = {{INF,-1,0,INF},{INF,INF,INF,-1},{INF,-1,INF,-1},{0,-1,INF,INF}};
    wallsAndGates(rooms);
    for (auto& row : rooms) { for (int v : row) cout << (v==INF?-1:v) << " "; cout << "\n"; }

    // Problem 10
    cout << validTree(5, {{0,1},{0,2},{0,3},{1,4}}) << "\n"; // true

    return 0;
}
