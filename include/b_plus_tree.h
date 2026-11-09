#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <shared_mutex>
#include <functional>
#include <type_traits>
#include <sstream>
#include <iostream>

#include "config.h"
#include "page.h"
#include "rid.h"
#include "buffer_pool_manager.h"
#include "common_page_header.h"
#include "b_plus_tree_page.h"
#include "b_plus_tree_header_page.h"
#include "b_plus_tree_internal_page.h"
#include "b_plus_tree_leaf_page.h"
#include "index_iterator.h"
#include "generic_key.h"

/**
 * @struct TreeValidationResult
 * @brief Detailed diagnostic report returned by BPlusTree::ValidateTree().
 */
struct TreeValidationResult {
    bool is_valid{true};
    std::string error_message{};
    size_t total_keys{0};
    size_t leaf_page_count{0};
    size_t internal_page_count{0};
    uint32_t depth{0};

    explicit operator bool() const { return is_valid; }
};

/**
 * @class BPlusTree
 * @brief Buffer-pool-backed, disk-persisted B+ Tree index storage engine.
 *
 * Key Architecture Highlights:
 * 1. Persistent Root Anchor: Tracks root_page_id in a dedicated BPlusTreeHeaderPage,
 *    enabling immediate crash-restart recovery and immutable external index anchors.
 * 2. Hierarchical Binary Search Routing: Navigates from root internal page down to target
 *    leaf using O(log N) binary search at each internal level.
 * 3. Point Lookup (GetValue): Performs binary search at internal nodes to descend to leaf,
 *    then binary search on leaf entries to find the exact key.
 * 4. Strict Pin Management: Guarantees that EVERY page pinned during traversal (both internal
 *    and leaf nodes, as well as the header anchor) is ALWAYS unpinned before returning.
 */
template <typename KeyType, typename ValueType = RID, typename KeyComparator = std::less<KeyType>>
class BPlusTree {
public:
    using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>;
    using InternalPage = BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>;
    using INDEXITERATOR_TYPE = IndexIterator<KeyType, ValueType, KeyComparator>;
    using Iterator = INDEXITERATOR_TYPE;
    using const_iterator = INDEXITERATOR_TYPE;
    using iterator = INDEXITERATOR_TYPE;

    /**
     * @brief Construct a new BPlusTree.
     * Allocates an initial persistent BPlusTreeHeaderPage in BufferPoolManager.
     * @param name Diagnostic identifier name.
     * @param bpm Pointer to BufferPoolManager.
     * @param comparator Custom key comparator (defaults to std::less<KeyType>).
     * @param leaf_max_size Max entries in leaf nodes (0 for automatic page size calculation).
     * @param internal_max_size Max entries in internal nodes (0 for automatic page size calculation).
     */
    BPlusTree(std::string name, BufferPoolManager *bpm, const KeyComparator &comparator = KeyComparator{},
              int leaf_max_size = 0, int internal_max_size = 0);

    /**
     * @brief Open an existing BPlusTree attached to an already allocated header anchor page.
     * @param name Diagnostic identifier name.
     * @param header_page_id Physical page ID of existing BPlusTreeHeaderPage.
     * @param bpm Pointer to BufferPoolManager.
     * @param comparator Custom key comparator.
     * @param leaf_max_size Max entries in leaf nodes.
     * @param internal_max_size Max entries in internal nodes.
     */
    BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager *bpm,
              const KeyComparator &comparator = KeyComparator{},
              int leaf_max_size = 0, int internal_max_size = 0);

    /**
     * @brief Convenience constructor for quick testing.
     */
    explicit BPlusTree(BufferPoolManager *bpm, const KeyComparator &comparator = KeyComparator{})
        : BPlusTree("b_plus_tree", bpm, comparator) {}

    BPlusTree(BPlusTree &&) noexcept = default;
    BPlusTree &operator=(BPlusTree &&) noexcept = default;
    BPlusTree(const BPlusTree &) = delete;
    BPlusTree &operator=(const BPlusTree &) = delete;
    ~BPlusTree() = default;

    // ==================== Root & Metadata Accessors ====================

    /**
     * @brief Get the root page ID of the tree.
     * Reads from the persistent header page if present, or falls back to cached root_page_id_.
     * Always unpins the header page.
     * @return page_id_t Root page ID, or INVALID_PAGE_ID if empty.
     */
    page_id_t GetRootPageId() const;

    /**
     * @brief Update the root page ID of the tree.
     * Updates persistent header page if present, and updates in-memory cache.
     * Always unpins the header page.
     * @param root_page_id New root page ID.
     */
    void SetRootPageId(page_id_t root_page_id);

    /**
     * @brief Check whether the tree is empty.
     * @return true if root_page_id is INVALID_PAGE_ID, false otherwise.
     */
    bool IsEmpty() const;

    /**
     * @brief Get the physical page ID of the persistent header anchor page.
     */
    page_id_t GetHeaderPageId() const { return header_page_id_; }

    /**
     * @brief Get tree depth from header page.
     * Always unpins the header page.
     */
    uint32_t GetDepth() const;

    /**
     * @brief Get total active record count from header page.
     * Always unpins the header page.
     */
    uint64_t GetRecordCount() const;

    const std::string &GetName() const { return name_; }
    int GetLeafMaxSize() const { return leaf_max_size_; }
    int GetInternalMaxSize() const { return internal_max_size_; }

    // ==================== Tree Traversal ====================

    /**
     * @brief Find the leftmost leaf page starting from root by following child pointer 0 at each level.
     * All traversed internal pages are unpinned immediately.
     * The returned leaf page is pinned and must be unpinned by the caller.
     *
     * @return Page* Pointer to pinned leftmost leaf Page, or nullptr if tree is empty or fetch fails.
     */
    Page *FindLeftmostLeafPage() const;

    /**
     * @brief Find the leaf page that can contain the given search key.
     * Starts at the root page and traverses down to a leaf page via binary search on internal nodes.
     * Unpins all visited internal pages immediately upon advancing to the child.
     * Leaves the returned leaf page pinned with pin_count incremented by 1.
     *
     * @param key Search key.
     * @param left_most If true, routes to the leftmost leaf page directly.
     * @return Page* Pointer to pinned leaf Page, or nullptr if tree is empty or fetch fails.
     */
    Page *FindLeafPage(const KeyType &key, bool left_most = false) const;

    // ==================== Point Lookup (GetValue) ====================

    /**
     * @brief Point lookup: search for an exact key and retrieve its associated value.
     *
     * Traversal & Search Dynamics:
     * 1. Traverses from root to target leaf page using FindLeafPage (binary search at each internal node).
     * 2. If tree is empty or leaf cannot be reached, returns false.
     * 3. Casts memory frame to BPlusTreeLeafPage and executes binary search (leaf->Lookup).
     * 4. Copies result if found.
     * 5. ALWAYS unpins the leaf page frame before returning.
     *
     * @param key The key to search for.
     * @param[out] result Output pointer where found value (RID) is stored (ignored if nullptr).
     * @return true if key was found, false otherwise.
     */
    bool GetValue(const KeyType &key, ValueType *result) const;

    /**
     * @brief Point lookup storing result into std::vector (CMU 15-445 compatibility).
     * Always unpins all pages.
     * @param key The key to search for.
     * @param[out] result Output vector where found value is appended.
     * @return true if key was found, false otherwise.
     */
    bool GetValue(const KeyType &key, std::vector<ValueType> *result) const;

    /**
     * @brief Check whether a key exists in the B+ Tree.
     * @param key The key to search for.
     * @return true if key exists, false otherwise.
     */
    bool Contains(const KeyType &key) const;

    // ==================== Leaf Chain Sequential Iteration (IndexIterator) ====================

    /**
     * @brief Get iterator pointing to the first (smallest) record in the tree (leftmost leaf, index 0).
     * Returns End() if tree is empty.
     *
     * @return INDEXITERATOR_TYPE Iterator positioned at the first record, or End().
     */
    INDEXITERATOR_TYPE Begin() const;

    INDEXITERATOR_TYPE begin() const { return Begin(); }
    INDEXITERATOR_TYPE cbegin() const { return Begin(); }
    INDEXITERATOR_TYPE Begin() { return const_cast<const BPlusTree *>(this)->Begin(); }
    INDEXITERATOR_TYPE begin() { return Begin(); }

    /**
     * @brief Get iterator pointing to the first record with key >= search_key (lower bound).
     * Navigates down from root to the appropriate leaf page and searches for the lower bound.
     * If all keys in that leaf are < search_key, sequentially follows next_page_id in the leaf chain.
     * Returns End() if the tree is empty or all keys in the tree are < search_key.
     *
     * @param key Search key (lower bound).
     * @return INDEXITERATOR_TYPE Iterator positioned at the first qualifying record, or End().
     */
    INDEXITERATOR_TYPE Begin(const KeyType &key) const;

    INDEXITERATOR_TYPE Begin(const KeyType &key) {
        return const_cast<const BPlusTree *>(this)->Begin(key);
    }

    /**
     * @brief Get end-of-iteration sentinel iterator.
     *
     * @return INDEXITERATOR_TYPE Sentinel iterator.
     */
    INDEXITERATOR_TYPE End() const {
        return INDEXITERATOR_TYPE();
    }

    INDEXITERATOR_TYPE end() const { return End(); }
    INDEXITERATOR_TYPE cend() const { return End(); }
    INDEXITERATOR_TYPE End() { return INDEXITERATOR_TYPE(); }
    INDEXITERATOR_TYPE end() { return INDEXITERATOR_TYPE(); }

    // ==================== Range Scan ====================

    /**
     * @brief Range scan: retrieve all (key, value) pairs whose keys fall within [low_key, high_key].
     * @param low_key Lower bound key (inclusive).
     * @param high_key Upper bound key (inclusive).
     * @param[out] result Output vector where matching pairs are stored.
     * @return size_t Total number of matching elements found.
     */
    size_t RangeScan(const KeyType &low_key, const KeyType &high_key,
                     std::vector<std::pair<KeyType, ValueType>> *result) const;

    /**
     * @brief Range scan: retrieve all values (RIDs) whose keys fall within [low_key, high_key].
     * @param low_key Lower bound key (inclusive).
     * @param high_key Upper bound key (inclusive).
     * @param[out] result Output vector where matching values are stored.
     * @return size_t Total number of matching elements found.
     */
    size_t RangeScan(const KeyType &low_key, const KeyType &high_key,
                     std::vector<ValueType> *result) const;

    // ==================== Insertion & Tree Growth ====================

    /**
     * @brief Insert a (key, value) pair into the B+ Tree.
     * - If tree is empty, delegates to InsertIntoEmptyTree.
     * - If tree is non-empty, delegates to InsertIntoLeaf.
     * - Rejects duplicate keys, returning false and leaving the tree unchanged.
     * All pinned pages are cleanly unpinned.
     *
     * @param key Key to insert.
     * @param value Value to associate with key.
     * @return true on successful insert, false if key is a duplicate or BPM fails.
     */
    bool Insert(const KeyType &key, const ValueType &value);

    /**
     * @brief Insert a key-value pair into an empty B+ Tree.
     * Allocates the initial root leaf page, initializes it, updates the persistent
     * header page (root_page_id, depth=1, record_count=1), and unpins as dirty.
     *
     * @param key Search key.
     * @param value Associated value (RID).
     * @return true on success, false if tree is not empty or BPM fails to allocate page.
     */
    bool InsertIntoEmptyTree(const KeyType &key, const ValueType &value);

    /**
     * @brief Alias for InsertIntoEmptyTree.
     */
    bool StartNewTree(const KeyType &key, const ValueType &value);

    /**
     * @brief Insert a key-value pair into the target leaf page.
     * Traverses from root to target leaf node:
     * - If duplicate key is found: unpins the leaf page (is_dirty = false) and returns false.
     * - If leaf has available space: inserts maintaining sorted order, increments record count,
     *   unpins the leaf page (is_dirty = true) and returns true.
     * - If leaf is full: splits the leaf and propagates split upwards.
     *
     * @param key Key to insert.
     * @param value Associated value (RID).
     * @return true on successful insertion, false on duplicate or error.
     */
    bool InsertIntoLeaf(const KeyType &key, const ValueType &value);

    /**
     * @brief Create a new root page when the current root splits (root split).
     *
     * Handles both leaf root splits and internal root splits:
     * 1. Allocates a new internal root page via BufferPoolManager.
     * 2. Initializes the new root with INVALID_PAGE_ID parent and internal_max_size_.
     * 3. Populates it with (old_root->GetPageId(), separator_key, new_sibling->GetPageId()).
     * 4. Updates parent pointers of old_root and new_sibling to new_root_pid.
     * 5. Sets root_page_id, increments depth in header page, and unpins new root as dirty.
     *
     * @param old_root Pointer to the old root page (either leaf or internal).
     * @param separator_key The separator key between old_root and new_sibling.
     * @param new_sibling Pointer to the newly allocated sibling page.
     * @return page_id_t The page ID of the newly created root, or INVALID_PAGE_ID on failure.
     */
    page_id_t CreateNewRoot(BPlusTreePage *old_root, const KeyType &separator_key, BPlusTreePage *new_sibling);

    /**
     * @brief Split a full leaf page:
     * 1. Allocates a new sibling leaf frame in BufferPoolManager.
     * 2. Moves the upper half of sorted entries from leaf to new_leaf.
     * 3. Fixes the sequential next-pointer chain: leaf -> new_leaf -> old_next.
     * The returned new leaf page frame is pinned in BPM and must be unpinned by the caller.
     *
     * @param leaf Pointer to the full leaf page.
     * @return LeafPage* Pointer to newly allocated sibling leaf (pinned in BPM), or nullptr on failure.
     */
    LeafPage *Split(LeafPage *leaf);

    /**
     * @brief Split an internal page into two halves:
     * 1. Allocates a new sibling internal frame in BufferPoolManager.
     * 2. Moves the upper half of entries from internal to new_internal via MoveHalfTo.
     * 3. Reparents all children moved to new_internal to point to new_internal->GetPageId().
     * The returned new internal page frame is pinned in BPM and must be unpinned by the caller.
     * Note: new_internal->KeyAt(0) contains the median separator key ready to be pushed to parent.
     *
     * @param internal Pointer to the full internal page.
     * @return InternalPage* Pointer to newly allocated sibling internal page (pinned in BPM), or nullptr on failure.
     */
    InternalPage *Split(InternalPage *internal);

    /**
     * @brief Split an internal page during insertion when it exceeds maximum capacity:
     * 1. Allocates a new sibling internal frame in BufferPoolManager.
     * 2. Forms a unified ordered sequence of existing child entries plus (separator_key, new_child_pid).
     * 3. Finds the median entry mid = total / 2; exports split_up_key = entries[mid].first.
     * 4. Keeps entries [0 .. mid - 1] in parent.
     * 5. Populates new_parent with entries [mid .. total - 1] (with entries[mid].second as leftmost child).
     * 6. Reparents all moved children to new_parent->GetPageId(), and reparents new_child_pid if it stayed in parent.
     * The returned new internal page frame is pinned in BPM and must be unpinned by the caller.
     *
     * @param parent Pointer to the full internal page being split.
     * @param old_child_pid Page ID of the existing child preceding the new split child.
     * @param separator_key Separator key for the new child.
     * @param new_child_pid Page ID of the newly split child.
     * @param split_up_key Out-parameter receiving the median separator key pushed to parent's parent.
     * @return InternalPage* Pointer to the newly allocated right sibling internal page, or nullptr on failure.
     */
    InternalPage *SplitInternal(InternalPage *parent, page_id_t old_child_pid, const KeyType &separator_key,
                                page_id_t new_child_pid, KeyType *split_up_key);

    /**
     * @brief Push the separator key and new child pointer up to the parent internal node.
     * - If old_child was root: invokes CreateNewRoot to allocate new internal root and increment depth.
     * - If parent has space: inserts separator key and new child pointer using InsertNodeAfter.
     * - If parent is full: invokes SplitInternal to split parent and recursively pushes up the median separator.
     * All pages fetched or allocated are cleanly unpinned.
     *
     * @param old_child Pointer to left child page.
     * @param separator_key Key separating old_child and new_child (first key of new_child for leaves).
     * @param new_child Pointer to newly created right child page.
     */
    void InsertIntoParent(BPlusTreePage *old_child, const KeyType &separator_key, BPlusTreePage *new_child);

    // ==================== Deletion & Tree Shrinking ====================

    /**
     * @brief Remove a key and its associated value from the B+ Tree.
     *
     * Execution Flow:
     * 1. Acquires unique (write) lock on tree rwlock.
     * 2. Checks if tree is empty; returns false immediately if empty.
     * 3. Traverses root-to-leaf via FindLeafPage to locate the leaf node that should contain key.
     * 4. Searches leaf via KeyIndex:
     *    - If key is absent (missing key): unpins the leaf page with is_dirty = false and returns false.
     * 5. If key is found:
     *    - Deletes record via RemoveAndDeleteRecord, shifting subsequent records left.
     *    - Decrements persistent index record count via UpdateRecordCount(-1).
     * 6. Underflow check:
     *    - If leaf is root:
     *      - If size == 0: frees root page via bpm_->DeletePage, resets root_page_id_ to INVALID_PAGE_ID,
     *        sets depth to 0 in header, and returns true.
     *      - If size > 0: unpins leaf as dirty and returns true.
     *    - If leaf is non-root:
     *      - If size >= leaf->GetMinSize() (leaf does NOT underflow):
     *        unpins leaf as dirty and returns true.
     *      - If size < leaf->GetMinSize() (underflow occurs):
     *        invokes CoalesceOrRedistribute hook, unpins leaf as dirty and returns true.
     * 7. Ensures zero pin leaks on all execution and error return paths.
     *
     * @param key Key to remove.
     * @return true if key was successfully removed, false if key was missing or tree empty.
     */
    bool Remove(const KeyType &key);

    /**
     * @brief Alias for Remove(const KeyType &key).
     */
    bool Delete(const KeyType &key);

    /**
     * @brief Borrow an entry from the left sibling into the underflowing leaf page and update the parent separator.
     *
     * 1. Moves the last (greatest) entry from left_sibling to the front (index 0) of leaf via MoveLastToFrontOf.
     * 2. Updates the separator key in parent at parent_idx to match the new first key of leaf (leaf->KeyAt(0)).
     *
     * @param leaf Pointer to the underflowing leaf page.
     * @param left_sibling Pointer to the left sibling leaf page with entries to spare.
     * @param parent Pointer to the shared parent internal page.
     * @param parent_idx Index of leaf in parent (parent_idx > 0).
     * @return true if borrowing succeeded, false otherwise.
     */
    bool RedistributeBorrowFromLeft(LeafPage *leaf, LeafPage *left_sibling, InternalPage *parent, int parent_idx);

    /**
     * @brief Borrow an entry from the right sibling into the underflowing leaf page and update the parent separator.
     *
     * 1. Moves the first (smallest) entry from right_sibling to the end of leaf via MoveFirstToEndOf.
     * 2. Updates the separator key in parent at parent_idx + 1 to match the new first key of right_sibling.
     * 3. If parent_idx > 0, also updates parent separator at parent_idx to leaf->KeyAt(0) in case leaf's
     *    first key was the one deleted.
     *
     * @param leaf Pointer to the underflowing leaf page.
     * @param right_sibling Pointer to the right sibling leaf page with entries to spare.
     * @param parent Pointer to the shared parent internal page.
     * @param parent_idx Index of leaf in parent (0 <= parent_idx < parent->GetSize() - 1).
     * @return true if borrowing succeeded, false otherwise.
     */
    bool RedistributeBorrowFromRight(LeafPage *leaf, LeafPage *right_sibling, InternalPage *parent, int parent_idx);

    /**
     * @brief Redistribute entries between an underflowing leaf page and one of its siblings sharing the same parent.
     *
     * @param leaf Pointer to underflowing leaf page.
     * @return true if redistribution resolved underflow, false if neither sibling could spare an entry.
     */
    bool Redistribute(LeafPage *leaf);

    /**
     * @brief Coalesce (merge) an underflowing leaf page with an adjacent sibling sharing the same parent.
     *
     * @param leaf Pointer to the underflowing leaf page.
     * @param[out] leaf_deleted Out-parameter set to true if leaf itself was the freed page.
     * @return true if merge succeeded, false otherwise.
     */
    bool Coalesce(LeafPage *leaf, bool *leaf_deleted = nullptr);

    /**
     * @brief Redistribute entries between an underflowing internal page and one of its siblings sharing the same parent.
     *
     * @param internal Pointer to underflowing internal page.
     * @return true if redistribution resolved underflow, false if neither sibling could spare an entry.
     */
    bool Redistribute(InternalPage *internal);

    /**
     * @brief Coalesce (merge) an underflowing internal page with an adjacent sibling sharing the same parent.
     *
     * @param internal Pointer to underflowing internal page.
     * @param[out] internal_deleted Out-parameter set to true if internal itself was freed.
     * @return true if merge succeeded, false otherwise.
     */
    bool Coalesce(InternalPage *internal, bool *internal_deleted = nullptr);

    /**
     * @brief Handle potential underflow in parent internal node after a child entry is removed.
     *
     * @param parent Pointer to parent internal page (pinned by caller).
     */
    void HandleParentUnderflow(InternalPage *parent);

    /**
     * @brief Adjust root page when root shrinks (internal root shrinks to 1 child, or leaf root becomes empty).
     *
     * @param old_root Pointer to root page.
     * @return true if root was adjusted, false otherwise.
     */
    bool AdjustRoot(BPlusTreePage *old_root);

    // ==================== Tree Validation & Graphviz Diagnostics ====================

    /**
     * @brief Validate all B+ Tree structural, occupancy, sorting, and topological invariants.
     *
     * @return TreeValidationResult Result containing validity boolean, error description, and stats.
     */
    TreeValidationResult ValidateTree() const;

    /**
     * @brief Convenience helper returning true if tree is valid, false otherwise.
     * @param error_out Optional string pointer to receive error description on failure.
     */
    bool CheckIntegrity(std::string *error_out = nullptr) const;

    /**
     * @brief Output the B+ Tree structure in Graphviz DOT format to an output stream.
     *
     * @param out Output stream.
     */
    void ToDot(std::ostream &out) const;

    /**
     * @brief Return the Graphviz DOT representation as a std::string.
     */
    std::string ToDot() const;

    /**
     * @brief Dump the Graphviz DOT representation to a file on disk.
     * @param filename Target file path.
     * @return true on success, false on I/O error.
     */
    bool DumpGraphviz(const std::string &filename) const;

private:
    bool CoalesceOrRedistribute(InternalPage *internal, bool *internal_deleted = nullptr);
    bool CoalesceOrRedistribute(LeafPage *leaf, bool *leaf_deleted = nullptr);
    bool InsertIntoEmptyTreeInternal(const KeyType &key, const ValueType &value);
    bool InsertIntoLeafInternal(const KeyType &key, const ValueType &value);
    void UpdateHeaderMetadata(page_id_t root_pid, uint32_t depth, int64_t record_count_delta);
    void UpdateRecordCount(int64_t delta);

    /**
     * @brief Universal 3-way comparator helper supporting both bool comp(a,b) and int comp(a,b).
     */
    template <typename Comp, typename K>
    static int Compare(const Comp &comp, const K &a, const K &b) {
        using RetType = std::invoke_result_t<Comp, K, K>;
        if constexpr (std::is_same_v<RetType, bool>) {
            if (comp(a, b)) return -1;
            if (comp(b, a)) return 1;
            return 0;
        } else {
            auto res = comp(a, b);
            if (res < 0) return -1;
            if (res > 0) return 1;
            return 0;
        }
    }

    template <typename T>
    static std::string FormatForDot(const T &val) {
        if constexpr (std::is_same_v<T, RID>) {
            return val.ToString();
        } else {
            std::ostringstream oss;
            oss << val;
            std::string s = oss.str();
            std::string escaped;
            for (char c : s) {
                if (c == '{' || c == '}' || c == '|' || c == '<' || c == '>' || c == '"' || c == '\\') {
                    escaped += '\\';
                }
                escaped += c;
            }
            return escaped;
        }
    }

    std::string name_;
    BufferPoolManager *bpm_{nullptr};
    page_id_t header_page_id_{INVALID_PAGE_ID};
    page_id_t root_page_id_{INVALID_PAGE_ID};
    KeyComparator comparator_{};
    int leaf_max_size_{0};
    int internal_max_size_{0};
    mutable std::unique_ptr<std::shared_mutex> rwlock_{std::make_unique<std::shared_mutex>()};
};
