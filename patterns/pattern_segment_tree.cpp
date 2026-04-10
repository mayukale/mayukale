/*
 * PATTERN: Segment Tree
 *
 * CONCEPT:
 * A binary tree over an array where each node stores an aggregate (sum, min,
 * max, GCD, …) over a range. Build in O(n), answer range queries in O(log n),
 * and perform point or range updates in O(log n). Lazy propagation handles
 * range updates efficiently by deferring work.
 *
 * TIME:  Build O(n), Query/Update O(log n)
 * SPACE: O(4n) for the tree array
 *
 * WHEN TO USE:
 * - Range sum/min/max queries with point updates
 * - Range update + range query (lazy propagation)
 * - "Count of elements in range satisfying property"
 * - Persistent queries, 2D segment trees
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// SEGMENT TREE TEMPLATE (Sum)
// ─────────────────────────────────────────────
struct SegTree {
    int n;
    vector<long long> tree, lazy;
    SegTree(int n) : n(n), tree(4*n, 0), lazy(4*n, 0) {}
    void build(const vector<int>& arr, int node, int l, int r) {
        if (l == r) { tree[node] = arr[l]; return; }
        int mid = (l+r)/2;
        build(arr, 2*node, l, mid);
        build(arr, 2*node+1, mid+1, r);
        tree[node] = tree[2*node] + tree[2*node+1];
    }
    void pushDown(int node, int l, int r) {
        if (!lazy[node]) return;
        int mid = (l+r)/2;
        tree[2*node]   += lazy[node] * (mid-l+1);
        tree[2*node+1] += lazy[node] * (r-mid);
        lazy[2*node]   += lazy[node];
        lazy[2*node+1] += lazy[node];
        lazy[node] = 0;
    }
    // Range add [ql..qr] += val
    void update(int node, int l, int r, int ql, int qr, long long val) {
        if (qr < l || r < ql) return;
        if (ql <= l && r <= qr) {
            tree[node] += val * (r-l+1);
            lazy[node] += val;
            return;
        }
        pushDown(node, l, r);
        int mid = (l+r)/2;
        update(2*node, l, mid, ql, qr, val);
        update(2*node+1, mid+1, r, ql, qr, val);
        tree[node] = tree[2*node] + tree[2*node+1];
    }
    // Range sum query [ql..qr]
    long long query(int node, int l, int r, int ql, int qr) {
        if (qr < l || r < ql) return 0;
        if (ql <= l && r <= qr) return tree[node];
        pushDown(node, l, r);
        int mid = (l+r)/2;
        return query(2*node, l, mid, ql, qr) + query(2*node+1, mid+1, r, ql, qr);
    }
    void build(const vector<int>& arr) { build(arr, 1, 0, n-1); }
    void update(int l, int r, long long val) { update(1, 0, n-1, l, r, val); }
    long long query(int l, int r) { return query(1, 0, n-1, l, r); }
};

// ─────────────────────────────────────────────
// SEGMENT TREE — Range Min / Max
// ─────────────────────────────────────────────
struct MinSegTree {
    int n;
    vector<int> tree;
    MinSegTree(int n, const vector<int>& arr) : n(n), tree(4*n) { build(arr, 1, 0, n-1); }
    void build(const vector<int>& arr, int node, int l, int r) {
        if (l == r) { tree[node] = arr[l]; return; }
        int mid = (l+r)/2;
        build(arr, 2*node, l, mid); build(arr, 2*node+1, mid+1, r);
        tree[node] = min(tree[2*node], tree[2*node+1]);
    }
    void update(int node, int l, int r, int pos, int val) {
        if (l == r) { tree[node] = val; return; }
        int mid = (l+r)/2;
        if (pos <= mid) update(2*node, l, mid, pos, val);
        else            update(2*node+1, mid+1, r, pos, val);
        tree[node] = min(tree[2*node], tree[2*node+1]);
    }
    int query(int node, int l, int r, int ql, int qr) {
        if (qr < l || r < ql) return INT_MAX;
        if (ql <= l && r <= qr) return tree[node];
        int mid = (l+r)/2;
        return min(query(2*node,l,mid,ql,qr), query(2*node+1,mid+1,r,ql,qr));
    }
    void update(int pos, int val) { update(1, 0, n-1, pos, val); }
    int query(int l, int r) { return query(1, 0, n-1, l, r); }
};

// ─────────────────────────────────────────────
// PROBLEM 1: Range Sum Query — Mutable  [Difficulty: Medium]
// Source: LeetCode 307
// ─────────────────────────────────────────────
class NumArray {
    int n;
    vector<long long> tree;
    void update_(int node, int l, int r, int pos, int val) {
        if (l == r) { tree[node] = val; return; }
        int mid = (l+r)/2;
        if (pos<=mid) update_(2*node,l,mid,pos,val);
        else          update_(2*node+1,mid+1,r,pos,val);
        tree[node] = tree[2*node] + tree[2*node+1];
    }
    long long query_(int node, int l, int r, int ql, int qr) {
        if (qr<l||r<ql) return 0;
        if (ql<=l&&r<=qr) return tree[node];
        int mid=(l+r)/2;
        return query_(2*node,l,mid,ql,qr) + query_(2*node+1,mid+1,r,ql,qr);
    }
public:
    NumArray(const vector<int>& arr) : n((int)arr.size()), tree(4*arr.size(), 0) {
        for (int i=0;i<n;++i) update_(1,0,n-1,i,arr[i]);
    }
    void update(int i, int val) { update_(1,0,n-1,i,val); }
    int sumRange(int l, int r) { return (int)query_(1,0,n-1,l,r); }
};

// ─────────────────────────────────────────────
// PROBLEM 2: Count of Smaller Numbers After Self  [Difficulty: Hard]
// Source: LeetCode 315
// ─────────────────────────────────────────────
// Approach: Coordinate compress; process right-to-left; query smaller elements.
// Time: O(n log n)  Space: O(n)
vector<int> countSmaller(vector<int> nums) {
    int n = (int)nums.size();
    vector<int> sorted = nums; sort(sorted.begin(), sorted.end());
    sorted.erase(unique(sorted.begin(), sorted.end()), sorted.end());
    int m = (int)sorted.size();
    auto getIdx = [&](int v){ return (int)(lower_bound(sorted.begin(),sorted.end(),v)-sorted.begin())+1; };
    vector<int> bit(m+2, 0);
    auto add = [&](int i){ for (; i<=m; i+=i&(-i)) ++bit[i]; };
    auto sum = [&](int i){ int s=0; for (; i>0; i-=i&(-i)) s+=bit[i]; return s; };
    vector<int> result(n);
    for (int i = n-1; i >= 0; --i) {
        int idx = getIdx(nums[i]);
        result[i] = sum(idx-1);
        add(idx);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Range Min Query + Point Update  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// (Uses MinSegTree defined above)

// ─────────────────────────────────────────────
// PROBLEM 4: Number of Longest Increasing Subsequences (Seg Tree)  [Difficulty: Medium]
// Source: LeetCode 673 — O(n log n) via segment tree on values
// ─────────────────────────────────────────────
// Approach: Coordinate compress values; seg tree stores (maxLen, count) pairs.
// Time: O(n log n)  Space: O(n)
struct LenCount { int len; long long cnt; };
LenCount combine(LenCount a, LenCount b) {
    if (a.len > b.len) return a;
    if (b.len > a.len) return b;
    return {a.len, a.cnt + b.cnt};
}
struct LISSegTree {
    int n; vector<LenCount> tree;
    LISSegTree(int n) : n(n), tree(4*n, {0, 0}) {}
    void update(int node, int l, int r, int pos, LenCount val) {
        if (l==r) { tree[node]=combine(tree[node],val); return; }
        int mid=(l+r)/2;
        if (pos<=mid) update(2*node,l,mid,pos,val);
        else          update(2*node+1,mid+1,r,pos,val);
        tree[node]=combine(tree[2*node],tree[2*node+1]);
    }
    LenCount query(int node, int l, int r, int ql, int qr) {
        if (qr<l||r<ql) return {0,0};
        if (ql<=l&&r<=qr) return tree[node];
        int mid=(l+r)/2;
        return combine(query(2*node,l,mid,ql,qr),query(2*node+1,mid+1,r,ql,qr));
    }
    void update(int pos, LenCount val) { update(1,0,n-1,pos,val); }
    LenCount query(int l, int r) { return query(1,0,n-1,l,r); }
};

int findNumberOfLIS_segtree(vector<int> nums) {
    vector<int> sorted = nums; sort(sorted.begin(), sorted.end());
    sorted.erase(unique(sorted.begin(),sorted.end()),sorted.end());
    int m = (int)sorted.size();
    LISSegTree st(m);
    for (int x : nums) {
        int pos = (int)(lower_bound(sorted.begin(),sorted.end(),x)-sorted.begin());
        LenCount prev = (pos>0) ? st.query(0,pos-1) : LenCount{0,0};
        if (prev.cnt==0) prev={0,1};
        st.update(pos, {prev.len+1, prev.cnt});
    }
    auto res = st.query(0, m-1);
    return (int)res.cnt;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Falling Squares  [Difficulty: Hard]
// Source: LeetCode 699
// ─────────────────────────────────────────────
// Approach: Coordinate compress positions; segment tree for range max height.
// Time: O(n log n)  Space: O(n)
vector<int> fallingSquares(const vector<vector<int>>& positions) {
    // Coordinate compress
    vector<int> coords;
    for (auto& p : positions) { coords.push_back(p[0]); coords.push_back(p[0]+p[1]-1); }
    sort(coords.begin(), coords.end());
    coords.erase(unique(coords.begin(), coords.end()), coords.end());
    int m = (int)coords.size();
    auto compress = [&](int v){ return (int)(lower_bound(coords.begin(),coords.end(),v)-coords.begin()); };
    // Max segment tree with lazy
    vector<int> tree(4*m, 0), lazy(4*m, 0);
    function<void(int,int,int,int,int,int)> update = [&](int node,int l,int r,int ql,int qr,int val){
        if (qr<l||r<ql) return;
        if (ql<=l&&r<=qr) { tree[node]=max(tree[node],val); lazy[node]=max(lazy[node],val); return; }
        int mid=(l+r)/2;
        update(2*node,l,mid,ql,qr,val); update(2*node+1,mid+1,r,ql,qr,val);
        tree[node]=max(tree[2*node],tree[2*node+1]);
    };
    function<int(int,int,int,int,int)> query = [&](int node,int l,int r,int ql,int qr)->int{
        if (qr<l||r<ql) return 0;
        if (ql<=l&&r<=qr) return tree[node];
        // push down lazy
        if (lazy[node]) { tree[2*node]=max(tree[2*node],lazy[node]); lazy[2*node]=max(lazy[2*node],lazy[node]);
                          tree[2*node+1]=max(tree[2*node+1],lazy[node]); lazy[2*node+1]=max(lazy[2*node+1],lazy[node]); lazy[node]=0; }
        int mid=(l+r)/2;
        return max(query(2*node,l,mid,ql,qr),query(2*node+1,mid+1,r,ql,qr));
    };
    vector<int> result; int globalMax = 0;
    for (auto& p : positions) {
        int l=compress(p[0]), r=compress(p[0]+p[1]-1), side=p[1];
        int curMax = query(1,0,m-1,l,r);
        int newH = curMax + side;
        update(1,0,m-1,l,r,newH);
        globalMax = max(globalMax, newH);
        result.push_back(globalMax);
    }
    return result;
}

int main() {
    // Problem 1 (SegTree range add + sum)
    SegTree st(6);
    st.build({1,2,3,4,5,6});
    cout << st.query(1,4) << "\n"; // 14
    st.update(2,4,3);
    cout << st.query(1,4) << "\n"; // 23

    // Problem 1 (NumArray)
    NumArray na({1,3,5});
    cout << na.sumRange(0,2) << "\n"; // 9
    na.update(1,2);
    cout << na.sumRange(0,2) << "\n"; // 8

    // Problem 2
    auto p2 = countSmaller({5,2,6,1});
    for (int v : p2) cout << v << " "; cout << "\n"; // 2 1 1 0

    // Min tree
    MinSegTree mst(5, {3,1,4,1,5});
    cout << mst.query(0,4) << "\n"; // 1
    mst.update(1, 10);
    cout << mst.query(0,2) << "\n"; // 3

    // Problem 4
    cout << findNumberOfLIS_segtree({1,3,5,4,7}) << "\n"; // 2

    // Problem 5
    auto p5 = fallingSquares({{1,2},{2,3},{6,1}});
    for (int v : p5) cout << v << " "; cout << "\n"; // 2 5 5

    return 0;
}
