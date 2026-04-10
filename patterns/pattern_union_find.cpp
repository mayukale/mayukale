/*
 * PATTERN: Union-Find (Disjoint Set Union — DSU)
 *
 * CONCEPT:
 * Maintain a forest of trees where each tree represents a connected component.
 * `find(x)` returns the root of x's component with path compression.
 * `unite(x, y)` merges two components using union-by-rank/size.
 * After setup, queries for "are x and y connected?" are nearly O(1) amortized.
 *
 * TIME:  O(α(n)) per operation (inverse Ackermann — essentially O(1))
 * SPACE: O(n)
 *
 * WHEN TO USE:
 * - Dynamic connectivity queries
 * - Number of connected components / islands
 * - Minimum spanning tree (Kruskal's)
 * - Detect cycle in undirected graph
 * - "Accounts merge", "similar string groups"
 */

#include <bits/stdc++.h>
using namespace std;

// ─── DSU Template ─────────────────────────────
struct DSU {
    vector<int> parent, rank_, size_;
    int components;
    explicit DSU(int n) : parent(n), rank_(n, 0), size_(n, 1), components(n) {
        iota(parent.begin(), parent.end(), 0);
    }
    int find(int x) {
        if (parent[x] != x) parent[x] = find(parent[x]); // path compression
        return parent[x];
    }
    bool unite(int x, int y) {
        int rx = find(x), ry = find(y);
        if (rx == ry) return false;
        // Union by rank
        if (rank_[rx] < rank_[ry]) swap(rx, ry);
        parent[ry] = rx;
        size_[rx] += size_[ry];
        if (rank_[rx] == rank_[ry]) ++rank_[rx];
        --components;
        return true;
    }
    bool connected(int x, int y) { return find(x) == find(y); }
    int getSize(int x) { return size_[find(x)]; }
};

// ─────────────────────────────────────────────
// PROBLEM 1: Number of Connected Components  [Difficulty: Medium]
// Source: LeetCode 323
// ─────────────────────────────────────────────
// Approach: DSU; unite each edge; return components count.
// Time: O(n + E * α(n))  Space: O(n)
int countComponents(int n, const vector<vector<int>>& edges) {
    DSU dsu(n);
    for (auto& e : edges) dsu.unite(e[0], e[1]);
    return dsu.components;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Redundant Connection  [Difficulty: Medium]
// Source: LeetCode 684
// ─────────────────────────────────────────────
// Approach: Process edges; return the first edge that connects already-connected nodes.
// Time: O(E * α(n))  Space: O(n)
vector<int> findRedundantConnection(const vector<vector<int>>& edges) {
    int n = (int)edges.size();
    DSU dsu(n+1);
    for (auto& e : edges)
        if (!dsu.unite(e[0], e[1])) return e;
    return {};
}

// ─────────────────────────────────────────────
// PROBLEM 3: Number of Islands II (online)  [Difficulty: Hard]
// Source: LeetCode 305
// ─────────────────────────────────────────────
// Approach: Initially all water; add each land cell and merge with adjacent land.
// Time: O(k * α(m*n))  Space: O(m*n)
vector<int> numIslandsII(int m, int n, const vector<vector<int>>& positions) {
    DSU dsu(m * n);
    vector<bool> isLand(m*n, false);
    vector<int> result;
    const int dx[] = {0,0,1,-1}, dy[] = {1,-1,0,0};
    int islands = 0;
    for (auto& pos : positions) {
        int r = pos[0], c = pos[1], idx = r*n+c;
        if (isLand[idx]) { result.push_back(islands); continue; }
        isLand[idx] = true; ++islands;
        for (int d = 0; d < 4; ++d) {
            int nr = r+dx[d], nc = c+dy[d], nidx = nr*n+nc;
            if (nr>=0&&nr<m&&nc>=0&&nc<n&&isLand[nidx])
                if (dsu.unite(idx, nidx)) --islands;
        }
        result.push_back(islands);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Accounts Merge  [Difficulty: Medium]
// Source: LeetCode 721
// ─────────────────────────────────────────────
// Approach: DSU on email indices; merge accounts sharing emails.
// Time: O(N * L * α(N))  Space: O(N)
vector<vector<string>> accountsMerge(const vector<vector<string>>& accounts) {
    unordered_map<string, int> emailToId;
    unordered_map<int, string> idToName;
    int id = 0;
    for (auto& acc : accounts) {
        for (int i = 1; i < (int)acc.size(); ++i) {
            if (!emailToId.count(acc[i])) {
                emailToId[acc[i]] = id;
                idToName[id] = acc[0];
                ++id;
            }
        }
    }
    DSU dsu(id);
    for (auto& acc : accounts)
        for (int i = 2; i < (int)acc.size(); ++i)
            dsu.unite(emailToId[acc[1]], emailToId[acc[i]]);
    // Group by root
    unordered_map<int, vector<string>> groups;
    for (auto& [email, eid] : emailToId)
        groups[dsu.find(eid)].push_back(email);
    vector<vector<string>> result;
    for (auto& [root, emails] : groups) {
        sort(emails.begin(), emails.end());
        emails.insert(emails.begin(), idToName[root]);
        result.push_back(emails);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Kruskal's MST  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Sort edges by weight; greedily add edge if it connects different components.
// Time: O(E log E)  Space: O(V)
// Returns (total MST weight, edges in MST)
pair<int, vector<tuple<int,int,int>>> kruskal(int V, vector<tuple<int,int,int>> edges) {
    sort(edges.begin(), edges.end());
    DSU dsu(V);
    int totalWeight = 0;
    vector<tuple<int,int,int>> mst;
    for (auto& [w, u, v] : edges) {
        if (dsu.unite(u, v)) {
            totalWeight += w;
            mst.push_back({w, u, v});
        }
    }
    return {totalWeight, mst};
}

// ─────────────────────────────────────────────
// PROBLEM 6: Satisfiability of Equality Equations  [Difficulty: Medium]
// Source: LeetCode 990
// ─────────────────────────────────────────────
// Approach: Unite '==' pairs; then verify no '!=' pair has same root.
// Time: O(n * α(26))  Space: O(26)
bool equationsPossible(const vector<string>& equations) {
    DSU dsu(26);
    for (auto& eq : equations)
        if (eq[1]=='=') dsu.unite(eq[0]-'a', eq[3]-'a');
    for (auto& eq : equations)
        if (eq[1]=='!' && dsu.connected(eq[0]-'a', eq[3]-'a')) return false;
    return true;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Most Stones Removed with Same Row or Column  [Difficulty: Medium]
// Source: LeetCode 947
// ─────────────────────────────────────────────
// Approach: Unite stones sharing a row or column; answer = n - #components.
// Time: O(n² * α(n))  Space: O(n)
int removeStones(const vector<vector<int>>& stones) {
    int n = (int)stones.size();
    DSU dsu(n);
    for (int i = 0; i < n; ++i)
        for (int j = i+1; j < n; ++j)
            if (stones[i][0]==stones[j][0] || stones[i][1]==stones[j][1])
                dsu.unite(i, j);
    return n - dsu.components;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Swim in Rising Water  [Difficulty: Hard]
// Source: LeetCode 778
// ─────────────────────────────────────────────
// Approach: Sort cells by elevation; add in order until (0,0) and (n-1,n-1) are connected.
// Time: O(n² log n)  Space: O(n²)
int swimInWater(const vector<vector<int>>& grid) {
    int n = (int)grid.size();
    vector<pair<int,int>> cells(n*n);
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j)
        cells[grid[i][j]] = {i, j};
    DSU dsu(n*n);
    const int dx[] = {0,0,1,-1}, dy[] = {1,-1,0,0};
    for (int t = 0; t < n*n; ++t) {
        auto [r, c] = cells[t];
        for (int d = 0; d < 4; ++d) {
            int nr = r+dx[d], nc = c+dy[d];
            if (nr>=0&&nr<n&&nc>=0&&nc<n&&grid[nr][nc]<=t)
                dsu.unite(r*n+c, nr*n+nc);
        }
        if (dsu.connected(0, n*n-1)) return t;
    }
    return n*n-1;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Largest Component Size by Common Factor  [Difficulty: Hard]
// Source: LeetCode 952
// ─────────────────────────────────────────────
// Approach: For each number, unite it with each of its prime factors.
//           Answer = max component size.
// Time: O(n * sqrt(max_val))  Space: O(max_val)
int largestComponentSize(const vector<int>& nums) {
    int maxVal = *max_element(nums.begin(), nums.end());
    DSU dsu(maxVal+1);
    for (int n : nums) {
        for (int f = 2; (long long)f*f <= n; ++f) {
            if (n % f == 0) { dsu.unite(n, f); dsu.unite(n, n/f); }
        }
    }
    unordered_map<int,int> compCount;
    int result = 0;
    for (int n : nums) result = max(result, ++compCount[dsu.find(n)]);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Detect Cycle in Undirected Graph  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: For each edge, if both endpoints already connected → cycle.
// Time: O(E * α(V))  Space: O(V)
bool hasCycle(int V, const vector<vector<int>>& edges) {
    DSU dsu(V);
    for (auto& e : edges)
        if (!dsu.unite(e[0], e[1])) return true;
    return false;
}

int main() {
    // Problem 1
    cout << countComponents(5, {{0,1},{1,2},{3,4}}) << "\n"; // 2

    // Problem 2
    auto p2 = findRedundantConnection({{1,2},{1,3},{2,3}});
    cout << p2[0] << " " << p2[1] << "\n"; // 2 3

    // Problem 3
    auto p3 = numIslandsII(3, 3, {{0,0},{0,1},{1,2},{2,1},{1,1}});
    for (int v : p3) cout << v << " "; cout << "\n"; // 1 1 2 3 1

    // Problem 4
    auto p4 = accountsMerge({{"John","johnsmith@mail.com","john_newyork@mail.com"},
                              {"John","johnsmith@mail.com","john00@mail.com"},
                              {"Mary","mary@mail.com"},
                              {"John","johnnybravo@mail.com"}});
    cout << p4.size() << " accounts after merge\n"; // 3

    // Problem 5
    auto [w, mst] = kruskal(4, {{1,0,1},{2,1,2},{3,2,3},{4,0,3},{5,1,3},{6,0,2}});
    cout << "MST weight: " << w << "\n"; // 6

    // Problem 6
    cout << boolalpha << equationsPossible({"a==b","b!=a"}) << "\n"; // false
    cout << equationsPossible({"b==a","a==b"}) << "\n"; // true

    // Problem 7
    cout << removeStones({{0,0},{0,1},{1,0},{1,2},{2,1},{2,2}}) << "\n"; // 5

    // Problem 8
    cout << swimInWater({{0,2},{1,3}}) << "\n"; // 3

    // Problem 9
    cout << largestComponentSize({4,6,15,35}) << "\n"; // 4

    // Problem 10
    cout << hasCycle(4, {{0,1},{1,2},{2,3},{3,0}}) << "\n"; // true

    return 0;
}
