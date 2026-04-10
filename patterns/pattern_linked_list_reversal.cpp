/*
 * PATTERN: Linked List In-Place Reversal
 *
 * CONCEPT:
 * Reverse an entire linked list or a sub-portion using only O(1) extra space
 * by relinking nodes iteratively. The key is to track prev, current, and next
 * at each step. For k-group or sub-list reversals, combine with pointer math
 * to locate and re-attach the reversed segment.
 *
 * TIME:  O(n)
 * SPACE: O(1)
 *
 * WHEN TO USE:
 * - "Reverse a linked list" (whole or partial)
 * - "Reverse every k-group"
 * - Palindrome check, reorder list (second half reversed)
 * - Rotate list
 */

#include <bits/stdc++.h>
using namespace std;

// ─── Minimal node ─────────────────────────────
struct ListNode {
    int val;
    ListNode* next;
    explicit ListNode(int v, ListNode* n = nullptr) : val(v), next(n) {}
};

ListNode* build(initializer_list<int> vals) {
    ListNode dummy(0); ListNode* cur = &dummy;
    for (int v : vals) { cur->next = new ListNode(v); cur = cur->next; }
    return dummy.next;
}

void print(ListNode* head) {
    for (ListNode* n = head; n; n = n->next) cout << n->val << (n->next ? "->" : "\n");
    if (!head) cout << "(null)\n";
}

void freeList(ListNode* head) {
    while (head) { ListNode* t = head->next; delete head; head = t; }
}

// ─────────────────────────────────────────────
// PROBLEM 1: Reverse a Linked List  [Difficulty: Easy]
// Source: LeetCode 206
// ─────────────────────────────────────────────
// Approach: Iterative three-pointer reversal.
// Time: O(n)  Space: O(1)
ListNode* reverseList(ListNode* head) {
    ListNode* prev = nullptr, *curr = head;
    while (curr) {
        ListNode* nxt = curr->next;
        curr->next = prev;
        prev = curr;
        curr = nxt;
    }
    return prev;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Reverse a Sub-List (positions left..right)  [Difficulty: Medium]
// Source: LeetCode 92
// ─────────────────────────────────────────────
// Approach: Advance to left-1, reverse the segment, reconnect.
// Time: O(n)  Space: O(1)
ListNode* reverseBetween(ListNode* head, int left, int right) {
    ListNode dummy(0, head);
    ListNode* prev = &dummy;
    for (int i = 1; i < left; ++i) prev = prev->next;
    ListNode* curr = prev->next;
    for (int i = 0; i < right - left; ++i) {
        ListNode* nxt = curr->next;
        curr->next = nxt->next;
        nxt->next = prev->next;
        prev->next = nxt;
    }
    return dummy.next;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Reverse Nodes in k-Group  [Difficulty: Hard]
// Source: LeetCode 25
// ─────────────────────────────────────────────
// Approach: Count k nodes ahead; if available, reverse the group recursively.
// Time: O(n)  Space: O(n/k) recursion stack
ListNode* reverseKGroup(ListNode* head, int k) {
    ListNode* curr = head;
    int count = 0;
    while (curr && count < k) { curr = curr->next; ++count; }
    if (count < k) return head; // less than k nodes remain
    // Reverse k nodes
    ListNode* prev = nullptr, *node = head;
    for (int i = 0; i < k; ++i) {
        ListNode* nxt = node->next;
        node->next = prev;
        prev = node;
        node = nxt;
    }
    head->next = reverseKGroup(curr, k);
    return prev;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Rotate List  [Difficulty: Medium]
// Source: LeetCode 61
// ─────────────────────────────────────────────
// Approach: Find length, make circular, break at (n - k%n - 1) position.
// Time: O(n)  Space: O(1)
ListNode* rotateRight(ListNode* head, int k) {
    if (!head || !head->next || k == 0) return head;
    int n = 1;
    ListNode* tail = head;
    while (tail->next) { tail = tail->next; ++n; }
    k %= n;
    if (k == 0) return head;
    // New tail is at position n-k-1
    ListNode* newTail = head;
    for (int i = 0; i < n - k - 1; ++i) newTail = newTail->next;
    ListNode* newHead = newTail->next;
    newTail->next = nullptr;
    tail->next = head;
    return newHead;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Swap Nodes in Pairs  [Difficulty: Medium]
// Source: LeetCode 24
// ─────────────────────────────────────────────
// Approach: Iterative — for each pair, relink prev->second->first->next.
// Time: O(n)  Space: O(1)
ListNode* swapPairs(ListNode* head) {
    ListNode dummy(0, head);
    ListNode* prev = &dummy;
    while (prev->next && prev->next->next) {
        ListNode* a = prev->next;
        ListNode* b = a->next;
        a->next = b->next;
        b->next = a;
        prev->next = b;
        prev = a;
    }
    return dummy.next;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Reorder List  [Difficulty: Medium]
// Source: LeetCode 143
// ─────────────────────────────────────────────
// Approach: Find mid -> reverse second half -> merge two halves.
// Time: O(n)  Space: O(1)
void reorderList(ListNode* head) {
    if (!head || !head->next) return;
    // Step 1: Find middle
    ListNode* slow = head, *fast = head;
    while (fast->next && fast->next->next) {
        slow = slow->next; fast = fast->next->next;
    }
    // Step 2: Reverse second half
    ListNode* second = reverseList(slow->next);
    slow->next = nullptr;
    // Step 3: Merge
    ListNode* first = head;
    while (second) {
        ListNode* tmp1 = first->next, *tmp2 = second->next;
        first->next = second;
        second->next = tmp1;
        first = tmp1;
        second = tmp2;
    }
}

// ─────────────────────────────────────────────
// PROBLEM 7: Palindrome Linked List  [Difficulty: Easy]
// Source: LeetCode 234
// ─────────────────────────────────────────────
// Approach: Find mid, reverse second half, compare, restore.
// Time: O(n)  Space: O(1)
bool isPalindrome(ListNode* head) {
    if (!head || !head->next) return true;
    ListNode* slow = head, *fast = head;
    while (fast->next && fast->next->next) {
        slow = slow->next; fast = fast->next->next;
    }
    ListNode* second = reverseList(slow->next);
    ListNode* p1 = head, *p2 = second;
    bool ok = true;
    while (p2) {
        if (p1->val != p2->val) { ok = false; break; }
        p1 = p1->next; p2 = p2->next;
    }
    reverseList(second); // restore
    return ok;
}

// ─────────────────────────────────────────────
// PROBLEM 8: Reverse Alternating k-Group Nodes  [Difficulty: Medium]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: Reverse k nodes, skip k nodes, repeat.
// Time: O(n)  Space: O(1)
ListNode* reverseAlternatingKGroup(ListNode* head, int k) {
    ListNode* curr = head, *prev = nullptr, *conn = nullptr;
    ListNode* newHead = nullptr;
    bool reverseGroup = true;
    while (curr) {
        if (reverseGroup) {
            // Reverse k nodes
            ListNode* groupPrev = nullptr, *groupStart = curr;
            int cnt = 0;
            while (curr && cnt < k) {
                ListNode* nxt = curr->next;
                curr->next = groupPrev;
                groupPrev = curr;
                curr = nxt;
                ++cnt;
            }
            if (!newHead) newHead = groupPrev;
            if (conn) conn->next = groupPrev;
            conn = groupStart;
        } else {
            // Skip k nodes
            for (int i = 0; i < k && curr; ++i) {
                if (conn) { conn->next = curr; conn = conn->next; }
                curr = curr->next;
            }
        }
        reverseGroup = !reverseGroup;
    }
    if (conn) conn->next = nullptr;
    return newHead ? newHead : head;
}

// ─────────────────────────────────────────────
// PROBLEM 9: Remove Nth Node From End  [Difficulty: Medium]
// Source: LeetCode 19
// ─────────────────────────────────────────────
// Approach: Fast pointer advances n steps; then both move until fast reaches end.
// Time: O(n)  Space: O(1)
ListNode* removeNthFromEnd(ListNode* head, int n) {
    ListNode dummy(0, head);
    ListNode* fast = &dummy, *slow = &dummy;
    for (int i = 0; i <= n; ++i) fast = fast->next;
    while (fast) { slow = slow->next; fast = fast->next; }
    ListNode* toDelete = slow->next;
    slow->next = slow->next->next;
    delete toDelete;
    return dummy.next;
}

int main() {
    // Problem 1
    auto l1 = build({1,2,3,4,5});
    print(reverseList(l1)); // 5->4->3->2->1

    // Problem 2
    auto l2 = build({1,2,3,4,5});
    print(reverseBetween(l2, 2, 4)); // 1->4->3->2->5

    // Problem 3
    auto l3 = build({1,2,3,4,5});
    print(reverseKGroup(l3, 2)); // 2->1->4->3->5

    // Problem 4
    auto l4 = build({1,2,3,4,5});
    print(rotateRight(l4, 2)); // 4->5->1->2->3

    // Problem 5
    auto l5 = build({1,2,3,4});
    print(swapPairs(l5)); // 2->1->4->3

    // Problem 6
    auto l6 = build({1,2,3,4,5});
    reorderList(l6);
    print(l6); // 1->5->2->4->3

    // Problem 7
    auto l7 = build({1,2,2,1});
    cout << boolalpha << isPalindrome(l7) << "\n"; // true
    freeList(l7);

    // Problem 8
    auto l8 = build({1,2,3,4,5,6,7,8});
    print(reverseAlternatingKGroup(l8, 2)); // 2->1->3->4->6->5->7->8

    // Problem 9
    auto l9 = build({1,2,3,4,5});
    print(removeNthFromEnd(l9, 2)); // 1->2->3->5

    return 0;
}
