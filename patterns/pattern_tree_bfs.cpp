/*
 * PATTERN: Tree BFS (Level-Order Traversal)
 *
 * CONCEPT:
 * Use a queue to visit tree nodes level by level. At each iteration, process
 * all nodes at the current level before moving to the next. Snapshot the
 * queue size at the start of each level to know how many nodes belong to it.
 *
 * TIME:  O(n) — every node visited once
 * SPACE: O(w) — w = max width of the tree (queue holds at most one full level)
 *
 * WHEN TO USE:
 * - Level-order traversal, level averages/maximums
 * - Minimum depth, zigzag traversal
 * - Connect level-order neighbors
 * - Shortest path in unweighted graph/tree
 */

#include <bits/stdc++.h>
using namespace std;

// ─── Binary Tree Node ─────────────────────────
struct TreeNode {
    int val;
    TreeNode *left, *right;
    explicit TreeNode(int v, TreeNode* l = nullptr, TreeNode* r = nullptr)
        : val(v), left(l), right(r) {}
};

// Helper: build tree from level-order array (-1 = null)
TreeNode* build(const vector<int>& v) {
    if (v.empty() || v[0] == -1) return nullptr;
    TreeNode* root = new TreeNode(v[0]);
    queue<TreeNode*> q;
    q.push(root);
    int i = 1;
    while (!q.empty() && i < (int)v.size()) {
        auto* node = q.front(); q.pop();
        if (i < (int)v.size() && v[i] != -1) {
            node->left = new TreeNode(v[i]); q.push(node->left);
        } ++i;
        if (i < (int)v.size() && v[i] != -1) {
            node->right = new TreeNode(v[i]); q.push(node->right);
        } ++i;
    }
    return root;
}

// ─────────────────────────────────────────────
// PROBLEM 1: Binary Tree Level Order Traversal  [Difficulty: Medium]
// Source: LeetCode 102
// ─────────────────────────────────────────────
// Approach: BFS; snapshot level size, collect values row by row.
// Time: O(n)  Space: O(w)
vector<vector<int>> levelOrder(TreeNode* root) {
    if (!root) return {};
    vector<vector<int>> result;
    queue<TreeNode*> q;
    q.push(root);
    while (!q.empty()) {
        int sz = (int)q.size();
        vector<int> level;
        for (int i = 0; i < sz; ++i) {
            auto* node = q.front(); q.pop();
            level.push_back(node->val);
            if (node->left)  q.push(node->left);
            if (node->right) q.push(node->right);
        }
        result.push_back(level);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Binary Tree Level Order Traversal II (bottom-up)  [Difficulty: Medium]
// Source: LeetCode 107
// ─────────────────────────────────────────────
// Approach: BFS then reverse the result.
// Time: O(n)  Space: O(w)
vector<vector<int>> levelOrderBottom(TreeNode* root) {
    auto res = levelOrder(root);
    reverse(res.begin(), res.end());
    return res;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Binary Tree Zigzag Level Order  [Difficulty: Medium]
// Source: LeetCode 103
// ─────────────────────────────────────────────
// Approach: BFS; flip direction flag per level; reverse odd-indexed levels.
// Time: O(n)  Space: O(w)
vector<vector<int>> zigzagLevelOrder(TreeNode* root) {
    if (!root) return {};
    vector<vector<int>> result;
    queue<TreeNode*> q;
    q.push(root);
    bool leftToRight = true;
    while (!q.empty()) {
        int sz = (int)q.size();
        deque<int> level;
        for (int i = 0; i < sz; ++i) {
            auto* node = q.front(); q.pop();
            if (leftToRight) level.push_back(node->val);
            else              level.push_front(node->val);
            if (node->left)  q.push(node->left);
            if (node->right) q.push(node->right);
        }
        result.push_back(vector<int>(level.begin(), level.end()));
        leftToRight = !leftToRight;
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Average of Levels in Binary Tree  [Difficulty: Easy]
// Source: LeetCode 637
// ─────────────────────────────────────────────
// Approach: BFS per level, compute mean.
// Time: O(n)  Space: O(w)
vector<double> averageOfLevels(TreeNode* root) {
    if (!root) return {};
    vector<double> result;
    queue<TreeNode*> q;
    q.push(root);
    while (!q.empty()) {
        int sz = (int)q.size();
        double sum = 0;
        for (int i = 0; i < sz; ++i) {
            auto* node = q.front(); q.pop();
            sum += node->val;
            if (node->left)  q.push(node->left);
            if (node->right) q.push(node->right);
        }
        result.push_back(sum / sz);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Minimum Depth of Binary Tree  [Difficulty: Easy]
// Source: LeetCode 111
// ─────────────────────────────────────────────
// Approach: BFS; first leaf encountered is at minimum depth.
// Time: O(n)  Space: O(w)
int minDepth(TreeNode* root) {
    if (!root) return 0;
    queue<TreeNode*> q;
    q.push(root);
    int depth = 0;
    while (!q.empty()) {
        ++depth;
        int sz = (int)q.size();
        for (int i = 0; i < sz; ++i) {
            auto* node = q.front(); q.pop();
            if (!node->left && !node->right) return depth;
            if (node->left)  q.push(node->left);
            if (node->right) q.push(node->right);
        }
    }
    return depth;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Right Side View  [Difficulty: Medium]
// Source: LeetCode 199
// ─────────────────────────────────────────────
// Approach: BFS; last node at each level is visible from the right.
// Time: O(n)  Space: O(w)
vector<int> rightSideView(TreeNode* root) {
    if (!root) return {};
    vector<int> result;
    queue<TreeNode*> q;
    q.push(root);
    while (!q.empty()) {
        int sz = (int)q.size();
        for (int i = 0; i < sz; ++i) {
            auto* node = q.front(); q.pop();
            if (i == sz - 1) result.push_back(node->val);
            if (node->left)  q.push(node->left);
            if (node->right) q.push(node->right);
        }
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Connect Level Order Siblings (Populate Next Pointers)  [Difficulty: Medium]
// Source: LeetCode 116
// ─────────────────────────────────────────────
// Approach: BFS; connect each node to the next in queue within same level.
// Time: O(n)  Space: O(w)
struct Node116 {
    int val; Node116 *left, *right, *next;
    explicit Node116(int v) : val(v), left(nullptr), right(nullptr), next(nullptr) {}
};
Node116* connect(Node116* root) {
    if (!root) return nullptr;
    queue<Node116*> q;
    q.push(root);
    while (!q.empty()) {
        int sz = (int)q.size();
        for (int i = 0; i < sz; ++i) {
            auto* node = q.front(); q.pop();
            if (i < sz - 1) node->next = q.front();
            if (node->left)  q.push(node->left);
            if (node->right) q.push(node->right);
        }
    }
    return root;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Level Order Successor  [Difficulty: Easy]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: BFS; return the node enqueued immediately after target.
// Time: O(n)  Space: O(w)
TreeNode* levelOrderSuccessor(TreeNode* root, int key) {
    if (!root) return nullptr;
    queue<TreeNode*> q;
    q.push(root);
    while (!q.empty()) {
        auto* node = q.front(); q.pop();
        if (node->left)  q.push(node->left);
        if (node->right) q.push(node->right);
        if (node->val == key) return q.empty() ? nullptr : q.front();
    }
    return nullptr;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Find Largest Value in Each Tree Row  [Difficulty: Medium]
// Source: LeetCode 515
// ─────────────────────────────────────────────
// Approach: BFS; track max per level.
// Time: O(n)  Space: O(w)
vector<int> largestValues(TreeNode* root) {
    if (!root) return {};
    vector<int> result;
    queue<TreeNode*> q;
    q.push(root);
    while (!q.empty()) {
        int sz = (int)q.size(), mx = INT_MIN;
        for (int i = 0; i < sz; ++i) {
            auto* node = q.front(); q.pop();
            mx = max(mx, node->val);
            if (node->left)  q.push(node->left);
            if (node->right) q.push(node->right);
        }
        result.push_back(mx);
    }
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 10: Binary Tree Level Order Traversal (N-ary tree)  [Difficulty: Medium]
// Source: LeetCode 429
// ─────────────────────────────────────────────
struct NNode {
    int val;
    vector<NNode*> children;
    explicit NNode(int v) : val(v) {}
};
// Approach: BFS on N-ary tree; enqueue all children.
// Time: O(n)  Space: O(w)
vector<vector<int>> levelOrderNary(NNode* root) {
    if (!root) return {};
    vector<vector<int>> result;
    queue<NNode*> q;
    q.push(root);
    while (!q.empty()) {
        int sz = (int)q.size();
        vector<int> level;
        for (int i = 0; i < sz; ++i) {
            auto* node = q.front(); q.pop();
            level.push_back(node->val);
            for (auto* child : node->children) q.push(child);
        }
        result.push_back(level);
    }
    return result;
}

int main() {
    //       3
    //      / \
    //     9  20
    //        / \
    //       15   7
    auto root = build({3,9,20,-1,-1,15,7});

    // Problem 1
    auto p1 = levelOrder(root);
    for (auto& lv : p1) { for (int v : lv) cout << v << " "; cout << "\n"; }

    // Problem 2
    auto p2 = levelOrderBottom(root);
    for (auto& lv : p2) { for (int v : lv) cout << v << " "; cout << "\n"; }

    // Problem 3
    auto p3 = zigzagLevelOrder(root);
    for (auto& lv : p3) { for (int v : lv) cout << v << " "; cout << "\n"; }

    // Problem 4
    auto p4 = averageOfLevels(root);
    for (double v : p4) cout << v << " "; cout << "\n";

    // Problem 5
    cout << minDepth(root) << "\n"; // 2

    // Problem 6
    auto p6 = rightSideView(root);
    for (int v : p6) cout << v << " "; cout << "\n"; // 3 20 7

    // Problem 8
    auto succ = levelOrderSuccessor(root, 9);
    cout << (succ ? succ->val : -1) << "\n"; // 20

    // Problem 9
    auto p9 = largestValues(root);
    for (int v : p9) cout << v << " "; cout << "\n"; // 3 20 15

    // Problem 10 (N-ary)
    auto n1 = new NNode(1);
    auto n3 = new NNode(3); auto n2 = new NNode(2); auto n4 = new NNode(4);
    auto n5 = new NNode(5); auto n6 = new NNode(6);
    n3->children = {n5, n6}; n1->children = {n3, n2, n4};
    auto p10 = levelOrderNary(n1);
    for (auto& lv : p10) { for (int v : lv) cout << v << " "; cout << "\n"; }

    return 0;
}
