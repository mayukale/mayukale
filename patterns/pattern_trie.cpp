/*
 * PATTERN: Trie (Prefix Tree)
 *
 * CONCEPT:
 * A tree where each edge represents a character and each root-to-node path
 * forms a prefix. Nodes store an "end-of-word" flag. Enables O(L) insert,
 * search, and prefix-search where L = word length. Supports wildcard matching
 * and efficient prefix grouping.
 *
 * TIME:  O(L) per operation
 * SPACE: O(ALPHABET_SIZE * N * L) worst case
 *
 * WHEN TO USE:
 * - "Word search", "prefix matching", "autocomplete"
 * - Longest word, longest common prefix
 * - Word dictionary with wildcard search
 * - XOR maximization (binary trie)
 */

#include <bits/stdc++.h>
using namespace std;

// ─── Standard Trie Node ───────────────────────
struct TrieNode {
    array<TrieNode*, 26> children{};
    bool isEnd = false;
    int count = 0;   // words passing through this node
    ~TrieNode() { for (auto* c : children) delete c; }
};

// ─────────────────────────────────────────────
// PROBLEM 1: Implement Trie (Prefix Tree)  [Difficulty: Medium]
// Source: LeetCode 208
// ─────────────────────────────────────────────
// Approach: Array of 26 children per node; O(L) insert/search/startsWith.
// Time: O(L)  Space: O(26*N*L)
class Trie {
    TrieNode* root;
public:
    Trie() : root(new TrieNode()) {}
    ~Trie() { delete root; }
    void insert(const string& word) {
        TrieNode* node = root;
        for (char c : word) {
            int idx = c - 'a';
            if (!node->children[idx]) node->children[idx] = new TrieNode();
            node = node->children[idx];
            ++node->count;
        }
        node->isEnd = true;
    }
    bool search(const string& word) const {
        TrieNode* node = root;
        for (char c : word) {
            int idx = c - 'a';
            if (!node->children[idx]) return false;
            node = node->children[idx];
        }
        return node->isEnd;
    }
    bool startsWith(const string& prefix) const {
        TrieNode* node = root;
        for (char c : prefix) {
            int idx = c - 'a';
            if (!node->children[idx]) return false;
            node = node->children[idx];
        }
        return true;
    }
};

// ─────────────────────────────────────────────
// PROBLEM 2: Add and Search Words with Wildcards  [Difficulty: Medium]
// Source: LeetCode 211
// ─────────────────────────────────────────────
// Approach: Trie with DFS for '.' wildcard — try all children.
// Time: O(L) insert, O(26^L) worst search  Space: O(26*N*L)
class WordDictionary {
    TrieNode* root;
    bool dfs(TrieNode* node, const string& word, int idx) const {
        if (idx == (int)word.size()) return node->isEnd;
        char c = word[idx];
        if (c != '.') {
            int i = c - 'a';
            return node->children[i] && dfs(node->children[i], word, idx + 1);
        }
        for (auto* child : node->children)
            if (child && dfs(child, word, idx + 1)) return true;
        return false;
    }
public:
    WordDictionary() : root(new TrieNode()) {}
    ~WordDictionary() { delete root; }
    void addWord(const string& word) {
        TrieNode* node = root;
        for (char c : word) {
            int i = c - 'a';
            if (!node->children[i]) node->children[i] = new TrieNode();
            node = node->children[i];
        }
        node->isEnd = true;
    }
    bool search(const string& word) const { return dfs(root, word, 0); }
};

// ─────────────────────────────────────────────
// PROBLEM 3: Longest Word in Dictionary  [Difficulty: Easy]
// Source: LeetCode 720
// ─────────────────────────────────────────────
// Approach: Build trie; BFS/DFS keeping only words where every prefix exists.
// Time: O(N*L)  Space: O(N*L)
string longestWord(const vector<string>& words) {
    Trie trie;
    for (auto& w : words) trie.insert(w);
    string best;
    function<void(TrieNode*, string)> dfs = [&](TrieNode* node, string cur) {
        if (cur.size() > best.size() || (cur.size() == best.size() && cur < best))
            best = cur;
        for (int i = 0; i < 26; ++i) {
            if (node->children[i] && node->children[i]->isEnd)
                dfs(node->children[i], cur + char('a' + i));
        }
    };
    // Access root via a helper (re-implement inline for clarity)
    struct LocalTrie {
        TrieNode* root;
        void insert(const string& w) {
            TrieNode* n = root;
            for (char c : w) { int i=c-'a'; if (!n->children[i]) n->children[i]=new TrieNode(); n=n->children[i]; }
            n->isEnd = true;
        }
        string longest() {
            string best;
            function<void(TrieNode*, string)> dfs = [&](TrieNode* node, string cur) {
                if (cur.size() > best.size() || (cur.size()==best.size() && cur<best)) best = cur;
                for (int i = 0; i < 26; ++i)
                    if (node->children[i] && node->children[i]->isEnd)
                        dfs(node->children[i], cur + char('a'+i));
            };
            dfs(root, "");
            return best;
        }
    };
    LocalTrie lt; lt.root = new TrieNode();
    for (auto& w : words) lt.insert(w);
    return lt.longest();
}

// ─────────────────────────────────────────────
// PROBLEM 4: Word Search II  [Difficulty: Hard]
// Source: LeetCode 212
// ─────────────────────────────────────────────
// Approach: Build trie from word list; DFS on board using trie for pruning.
// Time: O(m*n*4^L)  Space: O(N*L)
vector<string> findWords(vector<vector<char>>& board, const vector<string>& words) {
    // Build trie
    TrieNode* root = new TrieNode();
    auto insert = [&](const string& w) {
        TrieNode* node = root;
        for (char c : w) {
            int i = c - 'a';
            if (!node->children[i]) node->children[i] = new TrieNode();
            node = node->children[i];
        }
        node->isEnd = true;
    };
    for (auto& w : words) insert(w);
    int m = (int)board.size(), n = (int)board[0].size();
    vector<string> result;
    const int dx[] = {0,0,1,-1}, dy[] = {1,-1,0,0};
    function<void(int,int,TrieNode*,string)> dfs = [&](int r, int c, TrieNode* node, string cur) {
        if (r<0||r>=m||c<0||c>=n||board[r][c]=='#') return;
        char ch = board[r][c];
        int idx = ch - 'a';
        if (!node->children[idx]) return;
        node = node->children[idx];
        cur += ch;
        if (node->isEnd) { result.push_back(cur); node->isEnd = false; } // avoid dups
        board[r][c] = '#';
        for (int d = 0; d < 4; ++d) dfs(r+dx[d], c+dy[d], node, cur);
        board[r][c] = ch;
    };
    for (int r = 0; r < m; ++r) for (int c = 0; c < n; ++c) dfs(r, c, root, "");
    delete root;
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Replace Words  [Difficulty: Medium]
// Source: LeetCode 648
// ─────────────────────────────────────────────
// Approach: Trie of roots; for each word in sentence, find shortest matching root.
// Time: O(N*L + S)  Space: O(N*L)
string replaceWords(const vector<string>& dictionary, const string& sentence) {
    TrieNode* root = new TrieNode();
    auto insert = [&](const string& w) {
        TrieNode* node = root;
        for (char c : w) {
            int i = c - 'a';
            if (!node->children[i]) node->children[i] = new TrieNode();
            node = node->children[i];
        }
        node->isEnd = true;
    };
    for (auto& d : dictionary) insert(d);
    auto findRoot = [&](const string& word) -> string {
        TrieNode* node = root;
        for (int i = 0; i < (int)word.size(); ++i) {
            int idx = word[i] - 'a';
            if (!node->children[idx]) return word;
            node = node->children[idx];
            if (node->isEnd) return word.substr(0, i + 1);
        }
        return word;
    };
    istringstream iss(sentence); string word, result;
    while (iss >> word) {
        if (!result.empty()) result += ' ';
        result += findRoot(word);
    }
    delete root;
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Maximum XOR of Two Numbers in Array (Binary Trie)  [Difficulty: Medium]
// Source: LeetCode 421
// ─────────────────────────────────────────────
// Approach: Build binary trie from nums; for each num greedily choose opposite bit.
// Time: O(n*32)  Space: O(n*32)
struct BinaryTrieNode {
    array<BinaryTrieNode*, 2> ch{};
    ~BinaryTrieNode() { delete ch[0]; delete ch[1]; }
};
int findMaximumXOR(const vector<int>& nums) {
    BinaryTrieNode* root = new BinaryTrieNode();
    auto insert = [&](int n) {
        BinaryTrieNode* node = root;
        for (int bit = 31; bit >= 0; --bit) {
            int b = (n >> bit) & 1;
            if (!node->ch[b]) node->ch[b] = new BinaryTrieNode();
            node = node->ch[b];
        }
    };
    auto query = [&](int n) -> int {
        BinaryTrieNode* node = root;
        int xorVal = 0;
        for (int bit = 31; bit >= 0; --bit) {
            int b = (n >> bit) & 1, want = 1 - b;
            if (node->ch[want]) { xorVal |= (1 << bit); node = node->ch[want]; }
            else node = node->ch[b];
        }
        return xorVal;
    };
    for (int n : nums) insert(n);
    int result = 0;
    for (int n : nums) result = max(result, query(n));
    delete root;
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Implement Trie with Delete  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Use count to track how many words pass each node; decrement on delete.
// Time: O(L)  Space: O(N*L)
class TrieWithDelete {
    struct Node {
        array<Node*, 26> ch{};
        int endCount = 0;
        int passCount = 0;
    };
    Node* root;
public:
    TrieWithDelete() : root(new Node()) {}
    void insert(const string& w) {
        Node* n = root;
        for (char c : w) {
            int i = c - 'a';
            if (!n->ch[i]) n->ch[i] = new Node();
            n = n->ch[i]; ++n->passCount;
        }
        ++n->endCount;
    }
    bool erase(const string& w) {
        if (!search(w)) return false;
        Node* n = root;
        for (char c : w) { int i=c-'a'; n=n->ch[i]; --n->passCount; }
        --n->endCount;
        return true;
    }
    bool search(const string& w) const {
        Node* n = root;
        for (char c : w) { int i=c-'a'; if (!n->ch[i]) return false; n=n->ch[i]; }
        return n->endCount > 0;
    }
};

int main() {
    // Problem 1
    Trie t;
    t.insert("apple");
    cout << boolalpha << t.search("apple") << "\n"; // true
    cout << t.search("app") << "\n"; // false
    cout << t.startsWith("app") << "\n"; // true
    t.insert("app");
    cout << t.search("app") << "\n"; // true

    // Problem 2
    WordDictionary wd;
    wd.addWord("bad"); wd.addWord("dad"); wd.addWord("mad");
    cout << wd.search("pad") << "\n"; // false
    cout << wd.search(".ad") << "\n"; // true
    cout << wd.search("b..") << "\n"; // true

    // Problem 3
    cout << longestWord({"w","wo","wor","word","world"}) << "\n"; // world

    // Problem 4
    vector<vector<char>> board = {{'o','a','a','n'},{'e','t','a','e'},{'i','h','k','r'},{'i','f','l','v'}};
    auto p4 = findWords(board, {"oath","pea","eat","rain"});
    for (auto& w : p4) cout << w << " "; cout << "\n"; // eat oath

    // Problem 5
    cout << replaceWords({"cat","bat","rat"}, "the cattle was rattled by the battery") << "\n";
    // the cat was rat by the bat

    // Problem 6
    cout << findMaximumXOR({3,10,5,25,2,8}) << "\n"; // 28

    // Problem 7
    TrieWithDelete twd;
    twd.insert("hello"); twd.insert("hello");
    cout << twd.erase("hello") << "\n"; // true
    cout << twd.search("hello") << "\n"; // true (still one copy)
    cout << twd.erase("hello") << "\n"; // true
    cout << twd.search("hello") << "\n"; // false

    return 0;
}
