/*
 * PATTERN: Fast & Slow Pointers (Floyd's Tortoise and Hare)
 *
 * CONCEPT:
 * Two pointers advance at different speeds through a sequence or linked list.
 * The slow pointer moves 1 step; the fast pointer moves 2 steps. If a cycle
 * exists they will meet; if not, fast reaches the end. Also useful for finding
 * the middle of a list, detecting entry point of a cycle, etc.
 *
 * TIME:  O(n)
 * SPACE: O(1)
 *
 * WHEN TO USE:
 * - Detect cycle in a linked list / array
 * - Find the start of a cycle
 * - Find the middle of a linked list
 * - "Happy number" / next-pointer functional graph problems
 */

#include <bits/stdc++.h>
using namespace std;

// ─── Minimal singly-linked list node ───────────
struct ListNode {
    int val;
    ListNode* next;
    explicit ListNode(int v, ListNode* n = nullptr) : val(v), next(n) {}
};

// Helper: build list from vector
ListNode* build(const vector<int>& vals, int cyclePos = -1) {
    if (vals.empty()) return nullptr;
    ListNode* head = new ListNode(vals[0]);
    ListNode* cur = head;
    ListNode* cycleEntry = (cyclePos == 0) ? head : nullptr;
    for (int i = 1; i < (int)vals.size(); ++i) {
        cur->next = new ListNode(vals[i]);
        cur = cur->next;
        if (i == cyclePos) cycleEntry = cur;
    }
    if (cyclePos >= 0) cur->next = cycleEntry; // create cycle
    return head;
}

// Helper: free non-cyclic list
void freeList(ListNode* head) {
    while (head) { ListNode* t = head->next; delete head; head = t; }
}

// ─────────────────────────────────────────────
// PROBLEM 1: Linked List Cycle Detection  [Difficulty: Easy]
// Source: LeetCode 141
// ─────────────────────────────────────────────
// Approach: Slow+fast; if they meet, cycle exists.
// Time: O(n)  Space: O(1)
bool hasCycle(ListNode* head) {
    ListNode* slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) return true;
    }
    return false;
}

// ─────────────────────────────────────────────
// PROBLEM 2: Find Cycle Start (Entry Point)  [Difficulty: Medium]
// Source: LeetCode 142
// ─────────────────────────────────────────────
// Approach: After meeting, reset slow to head; advance both at speed 1.
// Time: O(n)  Space: O(1)
ListNode* detectCycle(ListNode* head) {
    ListNode* slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            slow = head;
            while (slow != fast) { slow = slow->next; fast = fast->next; }
            return slow;
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────
// PROBLEM 3: Middle of the Linked List  [Difficulty: Easy]
// Source: LeetCode 876
// ─────────────────────────────────────────────
// Approach: When fast reaches end, slow is at middle (upper-mid for even).
// Time: O(n)  Space: O(1)
ListNode* middleNode(ListNode* head) {
    ListNode* slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    return slow;
}

// ─────────────────────────────────────────────
// PROBLEM 4: Happy Number  [Difficulty: Easy]
// Source: LeetCode 202
// ─────────────────────────────────────────────
// Approach: Digit-square sum forms a functional graph; cycle <=> not happy.
// Time: O(log n)  Space: O(1)
int digitSquareSum(int n) {
    int s = 0;
    while (n) { s += (n % 10) * (n % 10); n /= 10; }
    return s;
}
bool isHappy(int n) {
    int slow = n, fast = n;
    do {
        slow = digitSquareSum(slow);
        fast = digitSquareSum(digitSquareSum(fast));
    } while (slow != fast);
    return slow == 1;
}

// ─────────────────────────────────────────────
// PROBLEM 5: Find Duplicate Number  [Difficulty: Medium]
// Source: LeetCode 287
// ─────────────────────────────────────────────
// Approach: Treat array as a linked list: index -> nums[index]. Find cycle start.
// Time: O(n)  Space: O(1)
int findDuplicate(const vector<int>& nums) {
    int slow = nums[0], fast = nums[0];
    do {
        slow = nums[slow];
        fast = nums[nums[fast]];
    } while (slow != fast);
    slow = nums[0];
    while (slow != fast) {
        slow = nums[slow];
        fast = nums[fast];
    }
    return slow;
}

// ─────────────────────────────────────────────
// PROBLEM 6: Palindrome Linked List  [Difficulty: Easy]
// Source: LeetCode 234
// ─────────────────────────────────────────────
// Approach: Find mid, reverse second half, compare, restore.
// Time: O(n)  Space: O(1)
ListNode* reverseList(ListNode* head) {
    ListNode* prev = nullptr;
    while (head) {
        ListNode* nxt = head->next;
        head->next = prev;
        prev = head;
        head = nxt;
    }
    return prev;
}
bool isPalindromeList(ListNode* head) {
    ListNode* mid = middleNode(head);
    ListNode* secondHalf = reverseList(mid);
    ListNode* p1 = head, *p2 = secondHalf;
    bool result = true;
    while (p2) {
        if (p1->val != p2->val) { result = false; break; }
        p1 = p1->next; p2 = p2->next;
    }
    reverseList(secondHalf); // restore
    return result;
}

// ─────────────────────────────────────────────
// PROBLEM 7: Reorder List  [Difficulty: Medium]
// Source: LeetCode 143
// ─────────────────────────────────────────────
// Approach: Find mid, reverse second half, interleave.
// Time: O(n)  Space: O(1)
void reorderList(ListNode* head) {
    if (!head || !head->next) return;
    // Find mid
    ListNode* slow = head, *fast = head;
    while (fast->next && fast->next->next) {
        slow = slow->next; fast = fast->next->next;
    }
    // Reverse second half
    ListNode* second = reverseList(slow->next);
    slow->next = nullptr;
    // Interleave
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
// PROBLEM 8: Linked List Cycle Length  [Difficulty: Easy]
// Source: Classic
// ─────────────────────────────────────────────
// Approach: After detecting cycle, count steps to come back to meeting point.
// Time: O(n)  Space: O(1)
int cycleLength(ListNode* head) {
    ListNode* slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            int length = 1;
            ListNode* cur = slow->next;
            while (cur != slow) { ++length; cur = cur->next; }
            return length;
        }
    }
    return 0;
}

int main() {
    // Problem 1
    {
        ListNode* list = build({3,2,0,-4}, 1); // cycle at index 1
        cout << boolalpha << hasCycle(list) << "\n"; // true
        // Note: can't free cyclic list cleanly here; skip for brevity
    }

    // Problem 2
    {
        ListNode* list = build({3,2,0,-4}, 1);
        ListNode* entry = detectCycle(list);
        cout << (entry ? entry->val : -1) << "\n"; // 2
    }

    // Problem 3
    {
        ListNode* list = build({1,2,3,4,5});
        cout << middleNode(list)->val << "\n"; // 3
        freeList(list);
    }

    // Problem 4
    cout << isHappy(19) << "\n"; // true
    cout << isHappy(2)  << "\n"; // false

    // Problem 5
    cout << findDuplicate({1,3,4,2,2}) << "\n"; // 2

    // Problem 6
    {
        ListNode* list = build({1,2,2,1});
        cout << isPalindromeList(list) << "\n"; // true
        freeList(list);
    }

    // Problem 7
    {
        ListNode* list = build({1,2,3,4,5});
        reorderList(list);
        for (ListNode* n = list; n; n = n->next) cout << n->val << " ";
        cout << "\n"; // 1 5 2 4 3
        freeList(list);
    }

    // Problem 8
    {
        ListNode* list = build({1,2,3,4,5}, 2); // cycle starting at index 2
        cout << cycleLength(list) << "\n"; // 3
    }

    return 0;
}
