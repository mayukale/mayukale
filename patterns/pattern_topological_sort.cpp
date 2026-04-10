/*
 * PATTERN: Topological Sort
 *
 * CONCEPT:
 * For a Directed Acyclic Graph (DAG), topological order places each node
 * before all nodes it points to. Kahn's algorithm (BFS) tracks in-degrees;
 * nodes with in-degree 0 are sources. DFS-based topo sort post-order-pushes
 * to a stack. Cycle detection: if not all nodes are processed, a cycle exists.
 *
 * TIME:  O(V + E)
 * SPACE: O(V + E)
 *
 * WHEN TO USE:
 * - Course scheduling, task ordering with prerequisites
 * - Build dependency resolution
 * - Detect cycle in directed graph
 * - "Alien dictionary" — derive order from sorted word list
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Topological Sort (Kahn's BFS)  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Compute in-degrees; BFS from zero-in-degree nodes.
// Time: O(V+E)  Space: O(V+E)
vector<int> topoSort(int V, const vector<vector<int>>& adj) {
    vector<int> inDeg(V, 0);
    for (int u = 0; u < V; ++u)
        for (int v : adj[u]) ++inDeg[v];
    queue<int> q;
    for (int i = 0; i < V; ++i) if (inDeg[i] == 0) q.push(i);
    vector<int> order;
    while (!q.empty()) {
        int u = q.front(); q.pop();
        order.push_back(u);
        for (int v : adj[u]) if (--inDeg[v] == 0) q.push(v);
    }
    return (int)order.size() == V ? order : vector<int>{}; // empty = cycle
}

// ─────────────────────────────────────────────
// PROBLEM 2: Course Schedule I (can finish?)  [Difficulty: Medium]
// Source: LeetCode 207
// ─────────────────────────────────────────────
// Approach: Kahn's; if topo order includes all courses, no cycle.
// Time: O(V+E)  Space: O(V+E)
bool canFinish(int numCourses, const vector<vector<int>>& prerequisites) {
    vector<vector<int>> adj(numCourses);
    vector<int> inDeg(numCourses, 0);
    for (auto& p : prerequisites) { adj[p[1]].push_back(p[0]); ++inDeg[p[0]]; }
    queue<int> q;
    for (int i = 0; i < numCourses; ++i) if (inDeg[i] == 0) q.push(i);
    int done = 0;
    while (!q.empty()) {
        int u = q.front(); q.pop(); ++done;
        for (int v : adj[u]) if (--inDeg[v] == 0) q.push(v);
    }
    return done == numCourses;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Course Schedule II (actual order)  [Difficulty: Medium]
// Source: LeetCode 210
// ─────────────────────────────────────────────
// Approach: Kahn's; return the topo order (empty if cycle).
// Time: O(V+E)  Space: O(V+E)
vector<int> findOrder(int n, const vector<vector<int>>& prerequisites) {
    vector<vector<int>> adj(n);
    vector<int> inDeg(n, 0);
    for (auto& p : prerequisites) { adj[p[1]].push_back(p[0]); ++inDeg[p[0]]; }
    queue<int> q;
    for (int i = 0; i < n; ++i) if (inDeg[i] == 0) q.push(i);
    vector<int> order;
    while (!q.empty()) {
        int u = q.front(); q.pop();
        order.push_back(u);
        for (int v : adj[u]) if (--inDeg[v] == 0) q.push(v);
    }
    return (int)order.size() == n ? order : vector<int>{};
}

// ─────────────────────────────────────────────
// PROBLEM 4: Alien Dictionary  [Difficulty: Hard]
// Source: LeetCode 269
// ─────────────────────────────────────────────
// Approach: Compare adjacent words to derive edges; topo sort on letters.
// Time: O(C) where C = total characters  Space: O(1) (26 letters)
string alienOrder(const vector<string>& words) {
    unordered_map<char, unordered_set<char>> adj;
    unordered_map<char, int> inDeg;
    for (auto& w : words) for (char c : w) if (!inDeg.count(c)) inDeg[c] = 0;
    for (int i = 0; i + 1 < (int)words.size(); ++i) {
        const auto& w1 = words[i], &w2 = words[i+1];
        int len = min(w1.size(), w2.size());
        if (w1.size() > w2.size() && w1.substr(0, len) == w2) return ""; // invalid
        for (int j = 0; j < (int)len; ++j) {
            if (w1[j] != w2[j]) {
                if (!adj[w1[j]].count(w2[j])) {
                    adj[w1[j]].insert(w2[j]);
                    ++inDeg[w2[j]];
                }
                break;
            }
        }
    }
    queue<char> q;
    for (auto& [c, d] : inDeg) if (d == 0) q.push(c);
    string result;
    while (!q.empty()) {
        char c = q.front(); q.pop();
        result += c;
        for (char nb : adj[c]) if (--inDeg[nb] == 0) q.push(nb);
    }
    return (int)result.size() == (int)inDeg.size() ? result : "";
}

// ─────────────────────────────────────────────
// PROBLEM 5: Minimum Height Trees  [Difficulty: Medium]
// Source: LeetCode 310
// ─────────────────────────────────────────────
// Approach: Iteratively prune leaves (degree-1 nodes); roots are last survivors.
// Time: O(n)  Space: O(n)
vector<int> findMinHeightTrees(int n, const vector<vector<int>>& edges) {
    if (n == 1) return {0};
    vector<unordered_set<int>> adj(n);
    for (auto& e : edges) { adj[e[0]].insert(e[1]); adj[e[1]].insert(e[0]); }
    vector<int> leaves;
    for (int i = 0; i < n; ++i) if (adj[i].size() == 1) leaves.push_back(i);
    int remaining = n;
    while (remaining > 2) {
        remaining -= (int)leaves.size();
        vector<int> newLeaves;
        for (int leaf : leaves) {
            int nb = *adj[leaf].begin();
            adj[nb].erase(leaf);
            if (adj[nb].size() == 1) newLeaves.push_back(nb);
        }
        leaves = newLeaves;
    }
    return leaves;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Sequence Reconstruction  [Difficulty: Medium]
// Source: LeetCode 444
// ─────────────────────────────────────────────
// Approach: Build DAG from seqs; check topo sort gives exactly one order == org.
// Time: O(V+E)  Space: O(V+E)
bool sequenceReconstruction(const vector<int>& org, const vector<vector<int>>& seqs) {
    unordered_map<int, unordered_set<int>> adj;
    unordered_map<int, int> inDeg;
    for (auto& seq : seqs)
        for (int v : seq) if (!inDeg.count(v)) inDeg[v] = 0;
    for (auto& seq : seqs)
        for (int i = 0; i + 1 < (int)seq.size(); ++i)
            if (!adj[seq[i]].count(seq[i+1])) { adj[seq[i]].insert(seq[i+1]); ++inDeg[seq[i+1]]; }
    queue<int> q;
    for (auto& [v, d] : inDeg) if (d == 0) q.push(v);
    int idx = 0;
    while (!q.empty()) {
        if (q.size() > 1) return false; // ambiguous
        int u = q.front(); q.pop();
        if (idx >= (int)org.size() || org[idx++] != u) return false;
        for (int v : adj[u]) if (--inDeg[v] == 0) q.push(v);
    }
    return idx == (int)org.size();
}

// ─────────────────────────────────────────────
// PROBLEM 7: Parallel Courses (min semesters)  [Difficulty: Medium]
// Source: LeetCode 1136
// ─────────────────────────────────────────────
// Approach: Kahn's; count levels (semesters); detect cycle.
// Time: O(V+E)  Space: O(V+E)
int minimumSemesters(int n, const vector<vector<int>>& relations) {
    vector<vector<int>> adj(n + 1);
    vector<int> inDeg(n + 1, 0);
    for (auto& r : relations) { adj[r[0]].push_back(r[1]); ++inDeg[r[1]]; }
    queue<int> q;
    for (int i = 1; i <= n; ++i) if (inDeg[i] == 0) q.push(i);
    int semesters = 0, done = 0;
    while (!q.empty()) {
        ++semesters;
        int sz = (int)q.size();
        for (int i = 0; i < sz; ++i) {
            int u = q.front(); q.pop(); ++done;
            for (int v : adj[u]) if (--inDeg[v] == 0) q.push(v);
        }
    }
    return done == n ? semesters : -1;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Topological Sort (DFS-based)  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: DFS with visited/in-stack marks; push to stack on finish.
// Time: O(V+E)  Space: O(V+E)
bool dfsTopoSort(int u, const vector<vector<int>>& adj,
                  vector<int>& color, vector<int>& result) {
    // color: 0=white, 1=gray(in progress), 2=black(done)
    color[u] = 1;
    for (int v : adj[u]) {
        if (color[v] == 1) return false; // cycle
        if (color[v] == 0 && !dfsTopoSort(v, adj, color, result)) return false;
    }
    color[u] = 2;
    result.push_back(u);
    return true;
}
vector<int> topoSortDFS(int V, const vector<vector<int>>& adj) {
    vector<int> color(V, 0), result;
    for (int i = 0; i < V; ++i)
        if (color[i] == 0 && !dfsTopoSort(i, adj, color, result)) return {};
    reverse(result.begin(), result.end());
    return result;
}

int main() {
    // Problem 1
    vector<vector<int>> adj1(6);
    adj1[5].push_back(2); adj1[5].push_back(0);
    adj1[4].push_back(0); adj1[4].push_back(1);
    adj1[2].push_back(3); adj1[3].push_back(1);
    auto order = topoSort(6, adj1);
    for (int v : order) cout << v << " "; cout << "\n"; // 4 5 0 2 3 1 (one valid order)

    // Problem 2
    cout << boolalpha << canFinish(2, {{1,0}}) << "\n"; // true
    cout << canFinish(2, {{1,0},{0,1}}) << "\n"; // false

    // Problem 3
    auto p3 = findOrder(4, {{1,0},{2,0},{3,1},{3,2}});
    for (int v : p3) cout << v << " "; cout << "\n"; // 0 1 2 3

    // Problem 4
    cout << alienOrder({"wrt","wrf","er","ett","rftt"}) << "\n"; // wertf

    // Problem 5
    auto p5 = findMinHeightTrees(6, {{3,0},{3,1},{3,2},{3,4},{5,4}});
    for (int v : p5) cout << v << " "; cout << "\n"; // 3 4

    // Problem 6
    cout << sequenceReconstruction({1,2,3}, {{1,2},{1,3},{2,3}}) << "\n"; // true

    // Problem 7
    cout << minimumSemesters(3, {{1,3},{2,3}}) << "\n"; // 2

    // Problem 8
    auto p8 = topoSortDFS(6, adj1);
    for (int v : p8) cout << v << " "; cout << "\n";

    return 0;
}
