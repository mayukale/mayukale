# Interview Pattern Library — Index

> **31 patterns · 300+ problems · fully compilable C++**

---

## Pattern Files

| # | Pattern | File | Problems Covered | When to Use |
|---|---------|------|-----------------|-------------|
| 1 | **Sliding Window** | [pattern_sliding_window.cpp](pattern_sliding_window.cpp) | Max sum subarray, min length subarray, longest without repeating, K distinct, permutation in string, min window substring, sliding max, subarray count | Contiguous subarray/substring with size or value constraint |
| 2 | **Two Pointers** | [pattern_two_pointers.cpp](pattern_two_pointers.cpp) | Two/Three/Four sum, remove duplicates, container with most water, trapping rain water, squares sorted, move zeroes, palindrome, rescue boats | Sorted array pair/triplet search, or both-ends processing |
| 3 | **Fast & Slow Pointers** | [pattern_fast_slow_pointers.cpp](pattern_fast_slow_pointers.cpp) | Cycle detection, cycle start, middle of list, happy number, find duplicate, palindrome list, reorder list, cycle length | Cycle detection, middle of list, functional graph problems |
| 4 | **Merge Intervals** | [pattern_merge_intervals.cpp](pattern_merge_intervals.cpp) | Merge intervals, insert interval, intersections, meeting rooms I & II, non-overlapping, min arrows, employee free time, car pooling, min interval per query | Overlapping intervals, scheduling, gap finding |
| 5 | **Cyclic Sort** | [pattern_cyclic_sort.cpp](pattern_cyclic_sort.cpp) | Sort [1..n], missing number, disappeared numbers, duplicate, all duplicates, first missing positive, corrupt pair, k missing positives, set mismatch | Array of n numbers in range [1..n]; find missing/duplicate |
| 6 | **Linked List Reversal** | [pattern_linked_list_reversal.cpp](pattern_linked_list_reversal.cpp) | Reverse list, reverse sub-list, reverse k-group, rotate list, swap pairs, reorder list, palindrome, alternating k-group, remove nth from end | In-place linked list restructuring |
| 7 | **Tree BFS** | [pattern_tree_bfs.cpp](pattern_tree_bfs.cpp) | Level order, bottom-up, zigzag, averages, min depth, right side view, connect siblings, level successor, largest per row, N-ary level order | Level-by-level tree processing, shortest path |
| 8 | **Tree DFS** | [pattern_tree_dfs.cpp](pattern_tree_dfs.cpp) | Path sum, all paths, max path sum, diameter, balanced, LCA, validate BST, serialize/deserialize, good nodes, sum numbers, iterative inorder | Root-to-leaf paths, bottom-up aggregation, tree validation |
| 9 | **Two Heaps** | [pattern_two_heaps.cpp](pattern_two_heaps.cpp) | Median finder, sliding window median, IPO/maximize capital, task scheduler, meeting rooms III, K closest points | Streaming median, balance lower/upper halves |
| 10 | **Subsets** | [pattern_subsets.cpp](pattern_subsets.cpp) | Subsets, subsets with dups, permutations I & II, combination sum I & II, letter combinations, generate parentheses, palindrome partition, N-Queens | Exhaustive enumeration of subsets/permutations/combinations |
| 11 | **Modified Binary Search** | [pattern_modified_binary_search.cpp](pattern_modified_binary_search.cpp) | Classic BS, first/last position, rotated array, find min rotated, peak in mountain, search 2D matrix, kth smallest in matrix, sqrt, ship packages, koko bananas, kth distance | Sorted/partially sorted with find/count query; BS on answer |
| 12 | **Bitwise XOR** | [pattern_bitwise_xor.cpp](pattern_bitwise_xor.cpp) | Single number, two singles, missing number, missing+duplicate, complement, flip image, power of two, counting bits, XOR queries, min XOR sum, sum without + | One/two numbers appearing odd times; bit manipulation |
| 13 | **Top K Elements** | [pattern_top_k_elements.cpp](pattern_top_k_elements.cpp) | Kth largest, top K frequent, top K words, K closest, frequency sort, reorganize string, kth smallest matrix, K pairs min sum, connect sticks, nth ugly | Find k best elements by value or frequency |
| 14 | **K-way Merge** | [pattern_kway_merge.cpp](pattern_kway_merge.cpp) | Merge K lists, kth smallest in K arrays, smallest range K lists, merge two sorted, kth smallest pair sum, kth smallest matrix, merge K arrays, kth largest stream | Merge K sorted structures simultaneously |
| 15 | **Knapsack** | [pattern_knapsack.cpp](pattern_knapsack.cpp) | 0/1 knapsack, partition equal subset, subset sum count, target sum, min diff, coin change (min/count), unbounded knapsack, rod cutting, last stone weight, perfect squares | Items into capacity; reachability or optimality over a weight |
| 16 | **Topological Sort** | [pattern_topological_sort.cpp](pattern_topological_sort.cpp) | Kahn's BFS topo, course schedule I & II, alien dictionary, min height trees, sequence reconstruction, parallel courses, DFS topo sort | DAG ordering, prerequisite checking, cycle detection |
| 17 | **Graph Traversal** | [pattern_graph_traversal.cpp](pattern_graph_traversal.cpp) | Number of islands, shortest path binary matrix, clone graph, word ladder, bipartite check, pacific atlantic, rotting oranges, components, walls+gates, valid tree | Connected components, shortest path, grid flood fill |
| 18 | **Monotonic Stack** | [pattern_monotonic_stack.cpp](pattern_monotonic_stack.cpp) | Next greater I & II, daily temperatures, largest rectangle histogram, maximal rectangle, trapping rain, sum subarray mins, sliding max (deque), stock span, 132 pattern, remove K digits | Next greater/smaller element, histogram area, sliding extremes |
| 19 | **Trie** | [pattern_trie.cpp](pattern_trie.cpp) | Implement trie, wildcard search, longest word, word search II, replace words, max XOR (binary trie), trie with delete | Prefix matching, autocomplete, word dictionary, XOR maximization |
| 20 | **Greedy** | [pattern_greedy.cpp](pattern_greedy.cpp) | Jump game I & II, gas station, assign cookies, partition labels, candy, min platforms, lemonade change, min operations to zero, activity selection, hire K workers | Locally optimal choice leads to global optimum; often sort first |
| 21 | **Backtracking** | [pattern_backtracking.cpp](pattern_backtracking.cpp) | Combination sum, Sudoku solver, word search, permutations, N-Queens, restore IP, letter case permutation, rat in maze, count paths, palindrome partition | Try → recurse → undo; constraint satisfaction; generate all |
| 22 | **Divide & Conquer** | [pattern_divide_and_conquer.cpp](pattern_divide_and_conquer.cpp) | Merge sort, count inversions, max subarray D&C, quickselect, fast power, closest pair of points, different ways to parenthesize, beautiful array, build tree, median of two sorted arrays | Split into independent subproblems; combine results |
| 23 | **DP — 1D** | [pattern_dp_1d.cpp](pattern_dp_1d.cpp) | Climbing stairs, house robber I & II, max product subarray, LIS O(n log n), decode ways, word break, min cost tickets, max circular subarray, turbulent subarray, count LIS, wiggle subsequence | Single variable DP; Fibonacci-like recurrences |
| 24 | **DP — 2D** | [pattern_dp_2d.cpp](pattern_dp_2d.cpp) | Unique paths, min path sum, LCS, edit distance, longest palindromic substring, distinct subsequences, interleaving string, maximal square, dungeon game, min ASCII delete, longest common substring | Two sequences or grid; dp[i][j] = answer for subproblem (i,j) |
| 25 | **DP — Interval** | [pattern_dp_interval.cpp](pattern_dp_interval.cpp) | Matrix chain, burst balloons, palindrome partition II, count palindromic substrings, stone merging, unique BSTs, largest sum of averages, strange printer, predict the winner | dp[i][j] over a range; fill by increasing interval length |
| 26 | **DP — Digit** | [pattern_dp_digit.cpp](pattern_dp_digit.cpp) | Count unique digit numbers, monotone increasing digits, count digit ones, at-most-N from digit set, digit sum count, stepping numbers, rotated digits, XOR sum, consecutive diff numbers, nth digit | Count integers in [A,B] satisfying a digit-level constraint |
| 27 | **DP — Bitmask** | [pattern_dp_bitmask.cpp](pattern_dp_bitmask.cpp) | TSP (Held-Karp), min XOR sum assignment, hat wearing ways, can-I-win game, shortest path visiting all nodes, max students exam, stickers to spell, courier assignment, maximize score GCD pairs | Subset of n≤20 items as bitmask state; O(2^n * n) |
| 28 | **Union-Find** | [pattern_union_find.cpp](pattern_union_find.cpp) | Connected components, redundant connection, islands II, accounts merge, Kruskal's MST, equation satisfiability, most stones removed, swim in water, largest component by factor, cycle detection | Dynamic connectivity; merge components; O(α(n)) per op |
| 29 | **Segment Tree** | [pattern_segment_tree.cpp](pattern_segment_tree.cpp) | Range sum mutable, count smaller after self, range min update/query, count LIS with seg tree, falling squares | Range queries + point/range updates in O(log n) |
| 30 | **Fenwick Tree (BIT)** | [pattern_fenwick_tree.cpp](pattern_fenwick_tree.cpp) | Range sum mutable, count inversions, count smaller after self, reverse pairs, 2D range sum mutable, order statistics, subarray bounded max, count range sum | Prefix sum with point updates; simpler than segment tree |
| 31 | **String Matching** | [pattern_string_matching.cpp](pattern_string_matching.cpp) | KMP search, longest prefix-suffix, Z-search, repeated substring, Rabin-Karp, longest duplicate substring, find anagrams, shortest palindrome, minimum period, rotation check, Aho-Corasick multi-pattern | Find pattern in text in O(n+m); repeated/multiple patterns |

---

## Quick Reference by Problem Type

| Problem Clue | Pattern(s) |
|---|---|
| Contiguous subarray with constraint | Sliding Window |
| Sorted array pair/triplet/sum | Two Pointers |
| Cycle in linked list / array | Fast & Slow Pointers |
| Overlapping intervals, scheduling | Merge Intervals |
| Array with values in [1..n] | Cyclic Sort |
| In-place list restructuring | Linked List Reversal |
| Level-by-level tree | Tree BFS |
| Path sum, LCA, height | Tree DFS |
| Streaming median | Two Heaps |
| Generate all combinations/permutations | Subsets / Backtracking |
| Sorted structure, find/count | Modified Binary Search |
| Single element appearing once | Bitwise XOR |
| K best by value/frequency | Top K Elements |
| Merge K sorted lists | K-way Merge |
| Fit items into weight/budget | Knapsack DP |
| Task ordering with dependencies | Topological Sort |
| Connected components, flood fill | Graph Traversal (BFS/DFS) |
| Next greater/smaller element | Monotonic Stack |
| Prefix matching, autocomplete | Trie |
| Interval scheduling, minimize operations | Greedy |
| Find all valid arrangements | Backtracking |
| Sort, median, closest pair | Divide & Conquer |
| Fibonacci-like recurrence | DP — 1D |
| Two sequences, grid paths | DP — 2D |
| Optimal split of a range | DP — Interval |
| Count integers with digit property | DP — Digit |
| Assign n items optimally (n≤20) | DP — Bitmask |
| Dynamic connectivity | Union-Find |
| Range query + update | Segment Tree / Fenwick |
| Find pattern in string | String Matching (KMP/Z/RK) |

---

*Generated: 2026-04-09*
