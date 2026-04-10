/*
 * PATTERN: Tree DFS (Depth-First Search)
 *
 * CONCEPT:
 * Recursively (or with an explicit stack) explore as deep as possible down
 * each branch before backtracking. Three orderings: pre-order (root first),
 * in-order (left, root, right — gives sorted order in BST), post-order
 * (children first — useful for bottom-up aggregations like height/diameter).
 *
 * TIME:  O(n)
 * SPACE: O(h) — h = height of tree (O(log n) balanced, O(n) worst)
 *
 * WHEN TO USE:
 * - Path sum, path enumeration (root-to-leaf)
 * - Tree height, diameter, LCA
 * - Validate BST, serialize/deserialize
 * - Any problem requiring information bubbled up from children
 */

#include <bits/stdc++.h>
using namespace std;

struct TreeNode {
    int val;
    TreeNode *left, *right;
    explicit TreeNode(int v, TreeNode* l = nullptr, TreeNode* r = nullptr)
        : val(v), left(l), right(r) {}
};

TreeNode* build(const vector<int>& v) {
    if (v.empty() || v[0] == -1) return nullptr;
    TreeNode* root = new TreeNode(v[0]);
    queue<TreeNode*> q; q.push(root);
    int i = 1;
    while (!q.empty() && i < (int)v.size()) {
        auto* node = q.front(); q.pop();
        if (i < (int)v.size() && v[i] != -1) { node->left  = new TreeNode(v[i]); q.push(node->left); } ++i;
        if (i < (int)v.size() && v[i] != -1) { node->right = new TreeNode(v[i]); q.push(node->right); } ++i;
    }
    return root;
}

// ─────────────────────────────────────────────
// PROBLEM 1: Binary Tree Path Sum  [Difficulty: Easy]
// Source: LeetCode 112
// ─────────────────────────────────────────────
// Approach: DFS; subtract node value from target; at leaf check if remainder == 0.
// Time: O(n)  Space: O(h)
bool hasPathSum(TreeNode* root, int target) {
    if (!root) return false;
    if (!root->left && !root->right) return root->val == target;
    return hasPathSum(root->left,  target - root->val) ||
           hasPathSum(root->right, target - root->val);
}

// ─────────────────────────────────────────────
// PROBLEM 2: All Root-to-Leaf Paths with Sum  [Difficulty: Medium]
// Source: LeetCode 113
// ─────────────────────────────────────────────
// Approach: DFS with backtracking; record path when leaf sum matches.
// Time: O(n²)  Space: O(n·h)
void dfsPathSum(TreeNode* node, int rem, vector<int>& path,
                vector<vector<int>>& result) {
    if (!node) return;
    path.push_back(node->val);
    if (!node->left && !node->right && node->val == rem)
        result.push_back(path);
    dfsPathSum(node->left,  rem - node->val, path, result);
    dfsPathSum(node->right, rem - node->val, path, result);
    path.pop_back();
}
vector<vector<int>> pathSum(TreeNode* root, int target) {
    vector<vector<int>> result; vector<int> path;
    dfsPathSum(root, target, path, result);
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Binary Tree Maximum Path Sum  [Difficulty: Hard]
// Source: LeetCode 124
// ─────────────────────────────────────────────
// Approach: Post-order; at each node compute max gain through it; update global max.
// Time: O(n)  Space: O(h)
int maxPathSumHelper(TreeNode* node, int& globalMax) {
    if (!node) return 0;
    int left  = max(0, maxPathSumHelper(node->left,  globalMax));
    int right = max(0, maxPathSumHelper(node->right, globalMax));
    globalMax = max(globalMax, left + right + node->val);
    return node->val + max(left, right);
}
int maxPathSum(TreeNode* root) {
    int gmax = INT_MIN;
    maxPathSumHelper(root, gmax);
    return gmax;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Diameter of Binary Tree  [Difficulty: Easy]
// Source: LeetCode 543
// ─────────────────────────────────────────────
// Approach: Post-order height; diameter at node = left_height + right_height.
// Time: O(n)  Space: O(h)
int diameterHelper(TreeNode* node, int& diameter) {
    if (!node) return 0;
    int l = diameterHelper(node->left,  diameter);
    int r = diameterHelper(node->right, diameter);
    diameter = max(diameter, l + r);
    return 1 + max(l, r);
}
int diameterOfBinaryTree(TreeNode* root) {
    int d = 0; diameterHelper(root, d); return d;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Balanced Binary Tree  [Difficulty: Easy]
// Source: LeetCode 110
// ─────────────────────────────────────────────
// Approach: Post-order; return -1 if unbalanced, else height.
// Time: O(n)  Space: O(h)
int balancedHeight(TreeNode* node) {
    if (!node) return 0;
    int l = balancedHeight(node->left);
    int r = balancedHeight(node->right);
    if (l == -1 || r == -1 || abs(l - r) > 1) return -1;
    return 1 + max(l, r);
}
bool isBalanced(TreeNode* root) { return balancedHeight(root) != -1; }

// ─────────────────────────────────────────────
// PROBLEM 6: Lowest Common Ancestor  [Difficulty: Medium]
// Source: LeetCode 236
// ─────────────────────────────────────────────
// Approach: Post-order; if both subtrees return non-null, current node is LCA.
// Time: O(n)  Space: O(h)
TreeNode* lowestCommonAncestor(TreeNode* root, TreeNode* p, TreeNode* q) {
    if (!root || root == p || root == q) return root;
    auto* left  = lowestCommonAncestor(root->left,  p, q);
    auto* right = lowestCommonAncestor(root->right, p, q);
    if (left && right) return root;
    return left ? left : right;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Validate Binary Search Tree  [Difficulty: Medium]
// Source: LeetCode 98
// ─────────────────────────────────────────────
// Approach: Pass min/max bounds down; each node must be strictly inside.
// Time: O(n)  Space: O(h)
bool isValidBST(TreeNode* node, long lo = LONG_MIN, long hi = LONG_MAX) {
    if (!node) return true;
    if (node->val <= lo || node->val >= hi) return false;
    return isValidBST(node->left, lo, node->val) &&
           isValidBST(node->right, node->val, hi);
}

// ─────────────────────────────────────────────
// PROBLEM 8: Serialize and Deserialize Binary Tree  [Difficulty: Hard]
// Source: LeetCode 297
// ─────────────────────────────────────────────
// Approach: Pre-order DFS with "#" for null; deserialize with string stream.
// Time: O(n)  Space: O(n)
void serializeHelper(TreeNode* node, ostringstream& oss) {
    if (!node) { oss << "# "; return; }
    oss << node->val << " ";
    serializeHelper(node->left, oss);
    serializeHelper(node->right, oss);
}
string serialize(TreeNode* root) {
    ostringstream oss; serializeHelper(root, oss); return oss.str();
}
TreeNode* deserializeHelper(istringstream& iss) {
    string val; iss >> val;
    if (val == "#") return nullptr;
    auto* node = new TreeNode(stoi(val));
    node->left  = deserializeHelper(iss);
    node->right = deserializeHelper(iss);
    return node;
}
TreeNode* deserialize(const string& data) {
    istringstream iss(data); return deserializeHelper(iss);
}

// ─────────────────────────────────────────────
// PROBLEM 9: Count Good Nodes  [Difficulty: Medium]
// Source: LeetCode 1448
// ─────────────────────────────────────────────
// Approach: DFS with running max from root; node is good if val >= maxSoFar.
// Time: O(n)  Space: O(h)
int goodNodes(TreeNode* node, int maxSoFar = INT_MIN) {
    if (!node) return 0;
    int good = (node->val >= maxSoFar) ? 1 : 0;
    int newMax = max(maxSoFar, node->val);
    return good + goodNodes(node->left, newMax) + goodNodes(node->right, newMax);
}

// ─────────────────────────────────────────────
// PROBLEM 10: Sum Root to Leaf Numbers  [Difficulty: Medium]
// Source: LeetCode 129
// ─────────────────────────────────────────────
// Approach: DFS; accumulate number digit by digit; add at leaf.
// Time: O(n)  Space: O(h)
int sumNumbersDFS(TreeNode* node, int cur) {
    if (!node) return 0;
    cur = cur * 10 + node->val;
    if (!node->left && !node->right) return cur;
    return sumNumbersDFS(node->left, cur) + sumNumbersDFS(node->right, cur);
}
int sumNumbers(TreeNode* root) { return sumNumbersDFS(root, 0); }

// ─────────────────────────────────────────────
// PROBLEM 11: Inorder Traversal (Iterative)  [Difficulty: Easy]
// Source: LeetCode 94
// ─────────────────────────────────────────────
// Approach: Explicit stack; push left until null, process, go right.
// Time: O(n)  Space: O(h)
vector<int> inorderTraversal(TreeNode* root) {
    vector<int> result;
    stack<TreeNode*> stk;
    TreeNode* curr = root;
    while (curr || !stk.empty()) {
        while (curr) { stk.push(curr); curr = curr->left; }
        curr = stk.top(); stk.pop();
        result.push_back(curr->val);
        curr = curr->right;
    }
    return result;
}

int main() {
    //       5
    //      / \
    //     4   8
    //    /   / \
    //   11  13   4
    //  /  \       \
    // 7    2       1
    auto root = build({5,4,8,11,-1,13,4,7,2,-1,-1,-1,-1,-1,1});

    // Problem 1
    cout << boolalpha << hasPathSum(root, 22) << "\n"; // true

    // Problem 2
    auto p2 = pathSum(root, 22);
    for (auto& path : p2) { for (int v : path) cout << v << " "; cout << "\n"; }

    // Problem 3
    auto root3 = build({-3});
    cout << maxPathSum(root3) << "\n"; // -3

    // Problem 4
    cout << diameterOfBinaryTree(build({1,2,3,4,5})) << "\n"; // 3

    // Problem 5
    cout << isBalanced(build({3,9,20,-1,-1,15,7})) << "\n"; // true

    // Problem 6 (reuse root)
    // (LCA skipped for brevity — requires pointer identity)

    // Problem 7
    cout << isValidBST(build({2,1,3})) << "\n"; // true
    cout << isValidBST(build({5,1,4,-1,-1,3,6})) << "\n"; // false

    // Problem 8
    auto root8 = build({1,2,3,-1,-1,4,5});
    auto s = serialize(root8);
    cout << s << "\n";
    auto* r8 = deserialize(s);
    cout << serialize(r8) << "\n"; // same

    // Problem 9
    cout << goodNodes(build({3,1,4,3,-1,1,5})) << "\n"; // 4

    // Problem 10
    cout << sumNumbers(build({1,2,3})) << "\n"; // 25

    // Problem 11
    auto p11 = inorderTraversal(build({1,-1,2,-1,-1,3}));
    for (int v : p11) cout << v << " "; cout << "\n"; // 1 3 2 (example BST in-order)

    return 0;
}
