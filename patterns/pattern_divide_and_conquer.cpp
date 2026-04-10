/*
 * PATTERN: Divide & Conquer
 *
 * CONCEPT:
 * Recursively split the problem into smaller subproblems, solve each
 * independently, and combine results. The master theorem gives recurrences:
 * T(n) = aT(n/b) + O(n^d). Works best when subproblems don't overlap
 * (unlike DP). Classic examples: merge sort, quicksort, closest pair of points.
 *
 * TIME:  O(n log n) for merge/quick sort and many variants
 * SPACE: O(log n) to O(n) recursion stack
 *
 * WHEN TO USE:
 * - "Sort", "find median", "closest pair"
 * - "Maximum subarray" (Kadane is O(n) but D&C is O(n log n) teaching variant)
 * - "Count inversions", "multiply large numbers"
 * - Tree problems naturally split into left/right subtrees
 */

#include <bits/stdc++.h>
using namespace std;

// ─────────────────────────────────────────────
// PROBLEM 1: Merge Sort  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Split in half, sort each, merge.
// Time: O(n log n)  Space: O(n)
void mergeSort(vector<int>& arr, int l, int r) {
    if (l >= r) return;
    int mid = l + (r - l) / 2;
    mergeSort(arr, l, mid);
    mergeSort(arr, mid+1, r);
    vector<int> tmp;
    int i = l, j = mid+1;
    while (i <= mid && j <= r)
        tmp.push_back(arr[i] <= arr[j] ? arr[i++] : arr[j++]);
    while (i <= mid)  tmp.push_back(arr[i++]);
    while (j <= r)    tmp.push_back(arr[j++]);
    for (int k = l; k <= r; ++k) arr[k] = tmp[k-l];
}

// ─────────────────────────────────────────────
// PROBLEM 2: Count Inversions  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Merge sort variant; count cross-inversions during merge.
// Time: O(n log n)  Space: O(n)
long long mergeCount(vector<int>& arr, int l, int r) {
    if (l >= r) return 0;
    int mid = l + (r - l) / 2;
    long long inv = mergeCount(arr, l, mid) + mergeCount(arr, mid+1, r);
    vector<int> tmp;
    int i = l, j = mid+1;
    while (i <= mid && j <= r) {
        if (arr[i] <= arr[j]) tmp.push_back(arr[i++]);
        else { inv += mid - i + 1; tmp.push_back(arr[j++]); }
    }
    while (i <= mid) tmp.push_back(arr[i++]);
    while (j <= r)   tmp.push_back(arr[j++]);
    for (int k = l; k <= r; ++k) arr[k] = tmp[k-l];
    return inv;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Maximum Subarray (D&C)  [Difficulty: Medium]
// Source: LeetCode 53
// ─────────────────────────────────────────────
// Approach: Max subarray = max(left half, right half, crossing midpoint).
// Time: O(n log n)  Space: O(log n)
int maxCrossing(const vector<int>& nums, int l, int mid, int r) {
    int leftSum = INT_MIN, sum = 0;
    for (int i = mid; i >= l; --i) { sum += nums[i]; leftSum = max(leftSum, sum); }
    int rightSum = INT_MIN; sum = 0;
    for (int i = mid+1; i <= r; ++i) { sum += nums[i]; rightSum = max(rightSum, sum); }
    return leftSum + rightSum;
}
int maxSubarrayDC(const vector<int>& nums, int l, int r) {
    if (l == r) return nums[l];
    int mid = l + (r - l) / 2;
    return max({maxSubarrayDC(nums, l, mid),
                maxSubarrayDC(nums, mid+1, r),
                maxCrossing(nums, l, mid, r)});
}
int maxSubArray(const vector<int>& nums) {
    return maxSubarrayDC(nums, 0, (int)nums.size()-1);
}

// ─────────────────────────────────────────────
// PROBLEM 4: Quickselect — Kth Largest  [Difficulty: Medium]
// Source: LeetCode 215
// ─────────────────────────────────────────────
// Approach: Partition like quicksort; recurse only into the side containing k.
// Time: O(n) average, O(n²) worst  Space: O(log n)
int partition(vector<int>& nums, int l, int r) {
    int pivot = nums[r], i = l;
    for (int j = l; j < r; ++j) if (nums[j] >= pivot) swap(nums[i++], nums[j]);
    swap(nums[i], nums[r]);
    return i;
}
int quickSelect(vector<int>& nums, int l, int r, int k) {
    if (l == r) return nums[l];
    int pivIdx = partition(nums, l, r);
    if (pivIdx == k) return nums[k];
    return pivIdx < k ? quickSelect(nums, pivIdx+1, r, k)
                      : quickSelect(nums, l, pivIdx-1, k);
}
int findKthLargest(vector<int> nums, int k) {
    return quickSelect(nums, 0, (int)nums.size()-1, k-1);
}

// ─────────────────────────────────────────────
// PROBLEM 5: Pow(x, n)  [Difficulty: Medium]
// Source: LeetCode 50
// ─────────────────────────────────────────────
// Approach: Fast exponentiation: x^n = (x^(n/2))^2; handle negative n.
// Time: O(log n)  Space: O(log n)
double myPow(double x, long long n) {
    if (n < 0) { x = 1/x; n = -n; }
    if (n == 0) return 1;
    double half = myPow(x, n/2);
    return n % 2 == 0 ? half * half : half * half * x;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Closest Pair of Points  [Difficulty: Hard]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Sort by x; split, recurse, strip check by delta band.
// Time: O(n log² n)  Space: O(n)
struct Point { double x, y; };
double dist(const Point& a, const Point& b) {
    return sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y));
}
double stripClosest(vector<Point>& strip, double d) {
    sort(strip.begin(), strip.end(), [](const Point& a, const Point& b){ return a.y < b.y; });
    double min_d = d;
    for (int i = 0; i < (int)strip.size(); ++i)
        for (int j = i+1; j < (int)strip.size() && strip[j].y - strip[i].y < min_d; ++j)
            min_d = min(min_d, dist(strip[i], strip[j]));
    return min_d;
}
double closestPairHelper(vector<Point>& pts, int l, int r) {
    if (r - l <= 3) {
        double d = DBL_MAX;
        for (int i = l; i <= r; ++i) for (int j = i+1; j <= r; ++j) d = min(d, dist(pts[i], pts[j]));
        sort(pts.begin()+l, pts.begin()+r+1, [](const Point& a, const Point& b){ return a.y < b.y; });
        return d;
    }
    int mid = l + (r - l) / 2;
    double midX = pts[mid].x;
    double d = min(closestPairHelper(pts, l, mid), closestPairHelper(pts, mid+1, r));
    vector<Point> strip;
    for (int i = l; i <= r; ++i) if (abs(pts[i].x - midX) < d) strip.push_back(pts[i]);
    return min(d, stripClosest(strip, d));
}
double closestPair(vector<Point> pts) {
    sort(pts.begin(), pts.end(), [](const Point& a, const Point& b){ return a.x < b.x; });
    return closestPairHelper(pts, 0, (int)pts.size()-1);
}

// ─────────────────────────────────────────────
// PROBLEM 7: Different Ways to Add Parentheses  [Difficulty: Medium]
// Source: LeetCode 241
// ─────────────────────────────────────────────
// Approach: Split at each operator; D&C on left and right; combine all pairs.
// Time: O(n * Catalan(n))  Space: O(n²)
vector<int> diffWaysToCompute(const string& expr) {
    vector<int> result;
    for (int i = 0; i < (int)expr.size(); ++i) {
        char c = expr[i];
        if (c=='+' || c=='-' || c=='*') {
            auto left  = diffWaysToCompute(expr.substr(0, i));
            auto right = diffWaysToCompute(expr.substr(i+1));
            for (int l : left) for (int r : right) {
                if (c=='+') result.push_back(l+r);
                else if (c=='-') result.push_back(l-r);
                else result.push_back(l*r);
            }
        }
    }
    if (result.empty()) result.push_back(stoi(expr));
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Beautiful Array  [Difficulty: Medium]
// Source: LeetCode 932
// ─────────────────────────────────────────────
// Approach: D&C; odd-indexed and even-indexed elements maintain the property.
// Time: O(n log n)  Space: O(n)
vector<int> beautifulArray(int n) {
    vector<int> result = {1};
    while ((int)result.size() < n) {
        vector<int> tmp;
        for (int x : result) if (2*x-1 <= n) tmp.push_back(2*x-1);
        for (int x : result) if (2*x   <= n) tmp.push_back(2*x);
        result = tmp;
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Construct Binary Tree from Preorder and Inorder  [Difficulty: Medium]
// Source: LeetCode 105
// ─────────────────────────────────────────────
// Approach: Root = preorder[0]; split inorder around root recursively.
// Time: O(n)  Space: O(n)
struct TreeNode { int val; TreeNode *left, *right; explicit TreeNode(int v):val(v),left(nullptr),right(nullptr){} };
TreeNode* buildTree(const vector<int>& pre, int p1, int p2,
                    const vector<int>& ino, int i1, int i2,
                    unordered_map<int,int>& inMap) {
    if (p1 > p2 || i1 > i2) return nullptr;
    int rootVal = pre[p1];
    int inIdx = inMap[rootVal];
    int leftSize = inIdx - i1;
    auto* node = new TreeNode(rootVal);
    node->left  = buildTree(pre, p1+1,           p1+leftSize,  ino, i1,       inIdx-1,  inMap);
    node->right = buildTree(pre, p1+leftSize+1,  p2,           ino, inIdx+1,  i2,       inMap);
    return node;
}
TreeNode* buildTreeFromTraversal(vector<int> preorder, vector<int> inorder) {
    unordered_map<int,int> inMap;
    for (int i = 0; i < (int)inorder.size(); ++i) inMap[inorder[i]] = i;
    return buildTree(preorder, 0, (int)preorder.size()-1,
                     inorder,  0, (int)inorder.size()-1, inMap);
}

// ─────────────────────────────────────────────
// PROBLEM 10: Median of Two Sorted Arrays  [Difficulty: Hard]
// Source: LeetCode 4
// ─────────────────────────────────────────────
// Approach: Binary search on partition of smaller array; O(log min(m,n)).
// Time: O(log(min(m,n)))  Space: O(1)
double findMedianSortedArrays(vector<int> A, vector<int> B) {
    if (A.size() > B.size()) swap(A, B);
    int m = (int)A.size(), n = (int)B.size();
    int lo = 0, hi = m;
    while (lo <= hi) {
        int i = lo + (hi - lo) / 2;
        int j = (m + n + 1) / 2 - i;
        int maxLeftA  = (i == 0) ? INT_MIN : A[i-1];
        int minRightA = (i == m) ? INT_MAX : A[i];
        int maxLeftB  = (j == 0) ? INT_MIN : B[j-1];
        int minRightB = (j == n) ? INT_MAX : B[j];
        if (maxLeftA <= minRightB && maxLeftB <= minRightA) {
            if ((m + n) % 2 == 1) return max(maxLeftA, maxLeftB);
            return (max(maxLeftA, maxLeftB) + min(minRightA, minRightB)) / 2.0;
        } else if (maxLeftA > minRightB) hi = i - 1;
        else lo = i + 1;
    }
    return 0;
}

int main() {
    // Problem 1
    vector<int> arr = {38,27,43,3,9,82,10};
    mergeSort(arr, 0, (int)arr.size()-1);
    for (int v : arr) cout << v << " "; cout << "\n";

    // Problem 2
    vector<int> inv = {2,4,1,3,5};
    cout << mergeCount(inv, 0, (int)inv.size()-1) << "\n"; // 3

    // Problem 3
    cout << maxSubArray({-2,1,-3,4,-1,2,1,-5,4}) << "\n"; // 6

    // Problem 4
    cout << findKthLargest({3,2,1,5,6,4}, 2) << "\n"; // 5

    // Problem 5
    cout << fixed << setprecision(5) << myPow(2.0, 10) << "\n"; // 1024.00000

    // Problem 6
    vector<Point> pts = {{0,0},{3,4},{1,1},{5,2},{6,8}};
    cout << fixed << setprecision(5) << closestPair(pts) << "\n"; // ~1.41421

    // Problem 7
    auto p7 = diffWaysToCompute("2-1-1");
    for (int v : p7) cout << v << " "; cout << "\n"; // 0 2

    // Problem 8
    auto p8 = beautifulArray(5);
    for (int v : p8) cout << v << " "; cout << "\n"; // 1 5 3 2 4

    // Problem 9
    auto root9 = buildTreeFromTraversal({3,9,20,15,7}, {9,3,15,20,7});
    cout << root9->val << "\n"; // 3

    // Problem 10
    cout << findMedianSortedArrays({1,3}, {2}) << "\n"; // 2.0
    cout << findMedianSortedArrays({1,2}, {3,4}) << "\n"; // 2.5

    return 0;
}
