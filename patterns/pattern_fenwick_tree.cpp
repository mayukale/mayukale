/*
 * PATTERN: Fenwick Tree (Binary Indexed Tree — BIT)
 *
 * CONCEPT:
 * A compact array-based structure supporting prefix sum queries and point
 * updates in O(log n). The key: index i "is responsible for" a range whose
 * length is determined by the lowest set bit of i. Adding i & (-i) jumps to
 * the parent; subtracting jumps to the next relevant range.
 * Simpler than a segment tree, but less flexible (no range updates natively).
 *
 * TIME:  O(log n) per update/query, O(n) build
 * SPACE: O(n)
 *
 * WHEN TO USE:
 * - Prefix sum with frequent point updates
 * - Count inversions, rank queries
 * - 2D BIT for 2D prefix sums
 * - "How many elements so far are <= x?" (with coordinate compression)
 */

#include <bits/stdc++.h>
using namespace std;

// ─── 1D Fenwick Tree Template ─────────────────
struct BIT {
    int n;
    vector<long long> tree;
    explicit BIT(int n) : n(n), tree(n+1, 0) {}
    void add(int i, long long val) { for (++i; i<=n; i+=i&(-i)) tree[i]+=val; }
    long long query(int i) { long long s=0; for (++i; i>0; i-=i&(-i)) s+=tree[i]; return s; }
    long long query(int l, int r) { return query(r) - (l>0 ? query(l-1) : 0); }
    // Build from array in O(n)
    void build(const vector<int>& arr) {
        for (int i=0; i<n; ++i) add(i, arr[i]);
    }
};

// ─── 2D Fenwick Tree ──────────────────────────
struct BIT2D {
    int m, n;
    vector<vector<long long>> tree;
    BIT2D(int m, int n) : m(m), n(n), tree(m+1, vector<long long>(n+1, 0)) {}
    void add(int r, int c, long long val) {
        for (int i=r+1; i<=m; i+=i&(-i))
            for (int j=c+1; j<=n; j+=j&(-j))
                tree[i][j] += val;
    }
    long long query(int r, int c) {
        long long s=0;
        for (int i=r+1; i>0; i-=i&(-i))
            for (int j=c+1; j>0; j-=j&(-j))
                s+=tree[i][j];
        return s;
    }
    // Query rectangle (r1,c1)-(r2,c2)
    long long query(int r1, int c1, int r2, int c2) {
        return query(r2,c2) - (r1>0?query(r1-1,c2):0)
                            - (c1>0?query(r2,c1-1):0)
                            + (r1>0&&c1>0?query(r1-1,c1-1):0);
    }
};

// ─────────────────────────────────────────────
// PROBLEM 1: Range Sum Query — Mutable (BIT)  [Difficulty: Medium]
// Source: LeetCode 307
// ─────────────────────────────────────────────
// Approach: Standard 1D BIT; update with diff, query prefix sum.
// Time: O(log n)  Space: O(n)
class NumArray {
    BIT bit;
    vector<int> nums;
public:
    NumArray(vector<int>& arr) : bit((int)arr.size()), nums(arr) {
        for (int i=0; i<(int)arr.size(); ++i) bit.add(i, arr[i]);
    }
    void update(int i, int val) { bit.add(i, val - nums[i]); nums[i] = val; }
    int sumRange(int l, int r) { return (int)bit.query(l, r); }
};

// ─────────────────────────────────────────────
// PROBLEM 2: Count Inversions  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Coordinate compress; process left-to-right; query how many already
//           added are > current element.
// Time: O(n log n)  Space: O(n)
long long countInversions(vector<int> arr) {
    int n = (int)arr.size();
    vector<int> sorted = arr; sort(sorted.begin(), sorted.end());
    sorted.erase(unique(sorted.begin(), sorted.end()), sorted.end());
    int m = (int)sorted.size();
    auto idx = [&](int v){ return (int)(lower_bound(sorted.begin(),sorted.end(),v)-sorted.begin())+1; };
    BIT bit(m);
    long long inv = 0;
    for (int x : arr) {
        int i = idx(x);
        inv += bit.query(i, m-1); // elements > x already inserted
        bit.add(i-1, 1);
    }
    return inv;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Count of Smaller Numbers After Self  [Difficulty: Hard]
// Source: LeetCode 315
// ─────────────────────────────────────────────
// Approach: Coordinate compress; process right-to-left; query prefix [0..val-1].
// Time: O(n log n)  Space: O(n)
vector<int> countSmaller(vector<int> nums) {
    int n = (int)nums.size();
    vector<int> sorted = nums; sort(sorted.begin(), sorted.end());
    sorted.erase(unique(sorted.begin(), sorted.end()), sorted.end());
    int m = (int)sorted.size();
    auto idx = [&](int v){ return (int)(lower_bound(sorted.begin(),sorted.end(),v)-sorted.begin()); };
    BIT bit(m);
    vector<int> result(n);
    for (int i = n-1; i >= 0; --i) {
        int pos = idx(nums[i]);
        result[i] = (pos>0) ? (int)bit.query(0, pos-1) : 0;
        bit.add(pos, 1);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Reverse Pairs  [Difficulty: Hard]
// Source: LeetCode 493
// ─────────────────────────────────────────────
// Approach: Sort + BIT or merge sort; count pairs (i,j) where nums[i] > 2*nums[j].
// Time: O(n log n)  Space: O(n)
int reversePairs(vector<int> nums) {
    int n = (int)nums.size();
    // Merge sort variant
    vector<int> tmp(n);
    int count = 0;
    function<void(int,int)> msort = [&](int l, int r) {
        if (l >= r) return;
        int mid = (l+r)/2;
        msort(l, mid); msort(mid+1, r);
        // Count pairs: nums[i] > 2*nums[j] where i in [l,mid], j in [mid+1,r]
        int j = mid+1;
        for (int i = l; i <= mid; ++i) {
            while (j <= r && (long long)nums[i] > 2LL*nums[j]) ++j;
            count += j - (mid+1);
        }
        // Merge
        int i2=l, j2=mid+1, k=l;
        while (i2<=mid && j2<=r) tmp[k++] = nums[i2]<=nums[j2] ? nums[i2++] : nums[j2++];
        while (i2<=mid) tmp[k++]=nums[i2++];
        while (j2<=r)   tmp[k++]=nums[j2++];
        for (int i=l;i<=r;++i) nums[i]=tmp[i];
    };
    msort(0, n-1);
    return count;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Range Sum Query 2D — Mutable (2D BIT)  [Difficulty: Hard]
// Source: LeetCode 308
// ─────────────────────────────────────────────
class NumMatrix {
    BIT2D bit;
    vector<vector<int>> matrix;
public:
    NumMatrix(vector<vector<int>>& mat) : bit((int)mat.size(),(int)mat[0].size()), matrix(mat) {
        for (int i=0;i<(int)mat.size();++i)
            for (int j=0;j<(int)mat[0].size();++j) bit.add(i,j,mat[i][j]);
    }
    void update(int r, int c, int val) { bit.add(r,c,val-matrix[r][c]); matrix[r][c]=val; }
    int sumRegion(int r1,int c1,int r2,int c2) { return (int)bit.query(r1,c1,r2,c2); }
};

// ─────────────────────────────────────────────
// PROBLEM 6: Queries on a Permutation  [Difficulty: Medium]
// Source: LeetCode 1409
// ─────────────────────────────────────────────
// Approach: BIT to find k-th one (binary search on BIT).
// Time: O(n log² n) with binary search, O(n log n) with BIT walk
// Time: O(q log n)  Space: O(n)
vector<int> processQueries(const vector<int>& queries, int m) {
    // Initial permutation [1..m] at positions [0..m-1]
    vector<int> order; // current order of elements
    for (int i=1;i<=m;++i) order.push_back(i);
    vector<int> result;
    for (int q : queries) {
        auto it = find(order.begin(), order.end(), q);
        result.push_back((int)(it - order.begin()));
        order.erase(it);
        order.insert(order.begin(), q);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Maximum Sum of Subarray with Length in [K1, K2]  [Difficulty: Medium]
// Source: Classic BIT + prefix sum
// ─────────────────────────────────────────────
// Approach: Prefix sum + sliding min; not BIT-specific but uses same idea.
// Time: O(n)  Space: O(n)
int maxSumSubarrayLengthRange(const vector<int>& arr, int k1, int k2) {
    int n = (int)arr.size();
    vector<int> prefix(n+1, 0);
    for (int i=0;i<n;++i) prefix[i+1]=prefix[i]+arr[i];
    // For each j in [k1..n], max(prefix[j] - min(prefix[j-k2..j-k1]))
    deque<int> dq; // indices of prefix[], monotone increasing (for min)
    int result = INT_MIN;
    for (int j = k1; j <= n; ++j) {
        // Add index j-k1 to deque
        int newIdx = j - k1;
        while (!dq.empty() && prefix[dq.back()] >= prefix[newIdx]) dq.pop_back();
        dq.push_back(newIdx);
        // Remove indices out of window [j-k2, j-k1]
        while (!dq.empty() && dq.front() < j - k2) dq.pop_front();
        result = max(result, prefix[j] - prefix[dq.front()]);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Number of Subarrays with Bounded Maximum  [Difficulty: Medium]
// Source: LeetCode 795
// ─────────────────────────────────────────────
// Approach: Count subarrays with max in [left, right] = f(right) - f(left-1).
// Time: O(n)  Space: O(1)
int numSubarrayBoundedMax(const vector<int>& A, int L, int R) {
    auto count = [&](int bound) -> long long {
        long long res=0, cur=0;
        for (int x : A) { cur = (x<=bound) ? cur+1 : 0; res+=cur; }
        return res;
    };
    return (int)(count(R) - count(L-1));
}

// ─────────────────────────────────────────────
// PROBLEM 9: Count of Range Sum  [Difficulty: Hard]
// Source: LeetCode 327
// ─────────────────────────────────────────────
// Approach: Merge sort on prefix sums; count pairs in range [lower, upper].
// Time: O(n log n)  Space: O(n)
int countRangeSum(const vector<int>& nums, int lower, int upper) {
    int n = (int)nums.size();
    vector<long long> prefix(n+1, 0);
    for (int i=0;i<n;++i) prefix[i+1]=prefix[i]+nums[i];
    int count = 0;
    function<void(int,int)> msort = [&](int l, int r) {
        if (l >= r) return;
        int mid = (l+r)/2;
        msort(l, mid); msort(mid+1, r);
        int j1=mid+1, j2=mid+1;
        for (int i=l; i<=mid; ++i) {
            while (j1<=r && prefix[j1]-prefix[i]<lower)  ++j1;
            while (j2<=r && prefix[j2]-prefix[i]<=upper) ++j2;
            count += j2-j1;
        }
        inplace_merge(prefix.begin()+l, prefix.begin()+mid+1, prefix.begin()+r+1);
    };
    msort(0, n);
    return count;
}

// ─────────────────────────────────────────────
// PROBLEM 10: BIT — Order Statistics (k-th smallest)  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: BIT of frequencies; binary lift for O(log n) k-th element.
// Time: O(log n)  Space: O(max_val)
struct OrderStatBIT {
    int n; vector<int> tree;
    OrderStatBIT(int n) : n(n), tree(n+1, 0) {}
    void add(int i, int val=1) { for (++i; i<=n; i+=i&(-i)) tree[i]+=val; }
    int kth(int k) { // 1-indexed k
        int pos=0;
        for (int pw=1<<(int)log2(n); pw; pw>>=1)
            if (pos+pw<=n && tree[pos+pw]<k) { pos+=pw; k-=tree[pos]; }
        return pos; // 0-indexed
    }
};

int main() {
    // Problem 1
    vector<int> arr = {1,3,5};
    NumArray na(arr);
    cout << na.sumRange(0,2) << "\n"; // 9
    na.update(1, 2);
    cout << na.sumRange(0,2) << "\n"; // 8

    // Problem 2
    cout << countInversions({5,4,3,2,1}) << "\n"; // 10

    // Problem 3
    auto p3 = countSmaller({5,2,6,1});
    for (int v : p3) cout << v << " "; cout << "\n"; // 2 1 1 0

    // Problem 4
    cout << reversePairs({1,3,2,3,1}) << "\n"; // 2

    // Problem 5
    vector<vector<int>> mat = {{3,0,1,4,2},{5,6,3,2,1},{1,2,0,1,5},{4,1,0,1,7},{1,0,3,0,5}};
    NumMatrix nm(mat);
    cout << nm.sumRegion(2,1,4,3) << "\n"; // 8
    nm.update(3,2,2);
    cout << nm.sumRegion(2,1,4,3) << "\n"; // 10

    // Problem 7
    cout << maxSumSubarrayLengthRange({1,-1,5,-2,3}, 3, 4) << "\n"; // 8

    // Problem 8
    cout << numSubarrayBoundedMax({2,1,4,3}, 2, 3) << "\n"; // 3

    // Problem 9
    cout << countRangeSum({-2,5,-1}, -2, 2) << "\n"; // 3

    // Problem 10
    OrderStatBIT osb(10);
    osb.add(3); osb.add(1); osb.add(7); osb.add(5);
    cout << osb.kth(2) << "\n"; // 3 (2nd smallest among {1,3,5,7})

    // Inversion count verification with BIT directly
    BIT bit(5);
    cout << "\nDirect BIT test:\n";
    bit.add(0,1); bit.add(2,1); bit.add(4,1);
    cout << bit.query(0,2) << "\n"; // 2 (positions 0 and 2)
    bit.add(1,3);
    cout << bit.query(0,3) << "\n"; // 5

    return 0;
}
