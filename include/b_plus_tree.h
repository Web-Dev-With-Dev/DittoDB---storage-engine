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
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <unordered_set>
#include <optional>
#include <iomanip>

#include "config.h"
#include "page.h"
#include "rid.h"
#include "buffer_pool_manager.h"
#include "common_page_header.h"
#include "b_plus_tree_page.h"
#include "b_plus_tree_header_page.h"
#include "b_plus_tree_internal_page.h"
#include "b_plus_tree_leaf_page.h"
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
              int leaf_max_size = 0, int internal_max_size = 0)
        : name_(std::move(name)), bpm_(bpm), comparator_(comparator) {
        
        leaf_max_size_ = (leaf_max_size > 0) ? leaf_max_size : static_cast<int>(LeafPage::LEAF_PAGE_SIZE);
        internal_max_size_ = (internal_max_size > 0) ? internal_max_size : static_cast<int>(InternalPage::INTERNAL_PAGE_SIZE);

        if (bpm_ != nullptr) {
            Page *header_frame = bpm_->NewPage(&header_page_id_);
            if (header_frame != nullptr) {
                auto *header = BPlusTreeHeaderPage::Cast(header_frame);
                header->Init(header_page_id_, INVALID_PAGE_ID, sizeof(KeyType));
                bpm_->UnpinPage(header_page_id_, true);
            }
        }
    }

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
              int leaf_max_size = 0, int internal_max_size = 0)
        : name_(std::move(name)), bpm_(bpm), header_page_id_(header_page_id), comparator_(comparator) {
        
        leaf_max_size_ = (leaf_max_size > 0) ? leaf_max_size : static_cast<int>(LeafPage::LEAF_PAGE_SIZE);
        internal_max_size_ = (internal_max_size > 0) ? internal_max_size : static_cast<int>(InternalPage::INTERNAL_PAGE_SIZE);
    }

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
    page_id_t GetRootPageId() const {
        if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
            Page *header_frame = bpm_->FetchPage(header_page_id_);
            if (header_frame != nullptr) {
                const auto *header = BPlusTreeHeaderPage::Cast(header_frame);
                page_id_t root_pid = header->GetRootPageId();
                bpm_->UnpinPage(header_page_id_, false);
                return root_pid;
            }
        }
        return root_page_id_;
    }

    /**
     * @brief Update the root page ID of the tree.
     * Updates persistent header page if present, and updates in-memory cache.
     * Always unpins the header page.
     * @param root_page_id New root page ID.
     */
    void SetRootPageId(page_id_t root_page_id) {
        root_page_id_ = root_page_id;
        if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
            Page *header_frame = bpm_->FetchPage(header_page_id_);
            if (header_frame != nullptr) {
                auto *header = BPlusTreeHeaderPage::Cast(header_frame);
                header->SetRootPageId(root_page_id);
                bpm_->UnpinPage(header_page_id_, true);
            }
        }
    }

    /**
     * @brief Check whether the tree is empty.
     * @return true if root_page_id is INVALID_PAGE_ID, false otherwise.
     */
    bool IsEmpty() const {
        return GetRootPageId() == INVALID_PAGE_ID;
    }

    /**
     * @brief Get the physical page ID of the persistent header anchor page.
     */
    page_id_t GetHeaderPageId() const {
        return header_page_id_;
    }

    /**
     * @brief Get tree depth from header page.
     * Always unpins the header page.
     */
    uint32_t GetDepth() const {
        if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
            Page *header_frame = bpm_->FetchPage(header_page_id_);
            if (header_frame != nullptr) {
                const auto *header = BPlusTreeHeaderPage::Cast(header_frame);
                uint32_t depth = header->GetDepth();
                bpm_->UnpinPage(header_page_id_, false);
                return depth;
            }
        }
        return (GetRootPageId() == INVALID_PAGE_ID) ? 0 : 1;
    }

    /**
     * @brief Get total active record count from header page.
     * Always unpins the header page.
     */
    uint64_t GetRecordCount() const {
        if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
            Page *header_frame = bpm_->FetchPage(header_page_id_);
            if (header_frame != nullptr) {
                const auto *header = BPlusTreeHeaderPage::Cast(header_frame);
                uint64_t count = header->GetRecordCount();
                bpm_->UnpinPage(header_page_id_, false);
                return count;
            }
        }
        return 0;
    }

    const std::string &GetName() const { return name_; }
    int GetLeafMaxSize() const { return leaf_max_size_; }
    int GetInternalMaxSize() const { return internal_max_size_; }

    // ==================== Tree Traversal ====================

    /**
     * @brief Traverse the tree from root to leaf node for a given search key.
     *
     * Traversal Algorithm:
     * 1. Fetches the root page into the buffer pool.
     * 2. Loops while the current node is an internal node:
     *    - Performs binary search via internal_page->Lookup(key, comparator) to select child pointer.
     *    - Fetches child page into the buffer pool.
     *    - IMMEDIATELY unpins the parent internal page frame.
     *    - Advances current pointer to child page.
     * 3. Upon reaching a leaf node, returns the pinned leaf page frame.
     *
     * Guarantee: Every internal page visited along the path is ALWAYS unpinned.
     * The returned leaf page is pinned and MUST be unpinned by the caller.
     *
     * @param key Search key guiding routing.
     * @param left_most If true, always routes to the leftmost child (ValueAt(0)).
     * @return Page* Pointer to pinned leaf Page, or nullptr if tree is empty or fetch fails.
     */
    Page *FindLeafPage(const KeyType &key, bool left_most = false) const {
        page_id_t curr_pid = GetRootPageId();
        if (curr_pid == INVALID_PAGE_ID || bpm_ == nullptr) {
            return nullptr;
        }

        Page *curr_page = bpm_->FetchPage(curr_pid);
        if (curr_page == nullptr) {
            return nullptr;
        }

        auto *tree_page = BPlusTreePage::Cast(curr_page);
        while (!tree_page->IsLeafPage()) {
            auto *internal_page = InternalPage::Cast(curr_page);
            page_id_t next_pid = INVALID_PAGE_ID;
            if (left_most) {
                next_pid = internal_page->ValueAt(0);
            } else {
                next_pid = internal_page->Lookup(key, comparator_);
            }

            if (next_pid == INVALID_PAGE_ID) {
                bpm_->UnpinPage(curr_pid, false);
                return nullptr;
            }

            Page *next_page = bpm_->FetchPage(next_pid);
            // Always unpin the parent internal page as soon as child is fetched
            bpm_->UnpinPage(curr_pid, false);

            if (next_page == nullptr) {
                return nullptr;
            }

            curr_page = next_page;
            curr_pid = next_pid;
            tree_page = BPlusTreePage::Cast(curr_page);
        }

        return curr_page;
    }

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
    bool GetValue(const KeyType &key, ValueType *result) const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);

        Page *leaf_page = FindLeafPage(key, false);
        if (leaf_page == nullptr) {
            return false;
        }

        auto *leaf = LeafPage::Cast(leaf_page);
        bool found = leaf->Lookup(key, result, comparator_);

        // ALWAYS unpin leaf page frame upon completion
        bpm_->UnpinPage(leaf_page->GetPageId(), false);

        return found;
    }

    /**
     * @brief Point lookup storing result into std::vector (CMU 15-445 compatibility).
     * Always unpins all pages.
     * @param key The key to search for.
     * @param[out] result Output vector where found value is appended.
     * @return true if key was found, false otherwise.
     */
    bool GetValue(const KeyType &key, std::vector<ValueType> *result) const {
        ValueType val;
        if (GetValue(key, &val)) {
            if (result != nullptr) {
                result->clear();
                result->push_back(val);
            }
            return true;
        }
        return false;
    }

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
    bool Insert(const KeyType &key, const ValueType &value) {
        std::unique_lock<std::shared_mutex> lock(*rwlock_);

        if (IsEmpty()) {
            return InsertIntoEmptyTreeInternal(key, value);
        }

        return InsertIntoLeafInternal(key, value);
    }

    /**
     * @brief Insert a key-value pair into an empty B+ Tree.
     * Allocates the initial root leaf page, initializes it, updates the persistent
     * header page (root_page_id, depth=1, record_count=1), and unpins as dirty.
     *
     * @param key Search key.
     * @param value Associated value (RID).
     * @return true on success, false if tree is not empty or BPM fails to allocate page.
     */
    bool InsertIntoEmptyTree(const KeyType &key, const ValueType &value) {
        std::unique_lock<std::shared_mutex> lock(*rwlock_);
        return InsertIntoEmptyTreeInternal(key, value);
    }

    /**
     * @brief Alias for InsertIntoEmptyTree.
     */
    bool StartNewTree(const KeyType &key, const ValueType &value) {
        return InsertIntoEmptyTree(key, value);
    }

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
    bool InsertIntoLeaf(const KeyType &key, const ValueType &value) {
        std::unique_lock<std::shared_mutex> lock(*rwlock_);
        return InsertIntoLeafInternal(key, value);
    }

private:
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

    /**
     * @brief Internal helper to initialize an empty tree by allocating the initial root leaf page.
     */
    bool InsertIntoEmptyTreeInternal(const KeyType &key, const ValueType &value) {
        if (!IsEmpty()) {
            return false;
        }

        page_id_t root_pid = INVALID_PAGE_ID;
        Page *root_frame = bpm_->NewPage(&root_pid);
        if (root_frame == nullptr) {
            return false;
        }

        auto *root = LeafPage::Cast(root_frame);
        root->Init(root_pid, INVALID_PAGE_ID, leaf_max_size_);
        root->Insert(key, value, comparator_);

        SetRootPageId(root_pid);
        UpdateHeaderMetadata(root_pid, 1, 1);

        bpm_->UnpinPage(root_pid, true);
        return true;
    }

    /**
     * @brief Internal helper to insert into target leaf page, splitting and propagating upwards if full.
     */
    bool InsertIntoLeafInternal(const KeyType &key, const ValueType &value) {
        if (IsEmpty()) {
            return InsertIntoEmptyTreeInternal(key, value);
        }

        Page *leaf_frame = FindLeafPage(key, false);
        if (leaf_frame == nullptr) {
            return false;
        }

        auto *leaf = LeafPage::Cast(leaf_frame);

        // Reject duplicates in unique index
        if (leaf->KeyIndex(key, comparator_) != -1) {
            bpm_->UnpinPage(leaf_frame->GetPageId(), false);
            return false;
        }

        // Case 1: Leaf has available capacity
        if (leaf->GetSize() < leaf->GetMaxSize()) {
            leaf->Insert(key, value, comparator_);
            UpdateRecordCount(1);
            bpm_->UnpinPage(leaf_frame->GetPageId(), true);
            return true;
        }

        // Case 2: Leaf is full -> Split Leaf Node!
        // 1. Allocate new leaf, move upper half, fix next-pointer chain
        LeafPage *new_leaf = Split(leaf);
        if (new_leaf == nullptr) {
            bpm_->UnpinPage(leaf_frame->GetPageId(), false);
            return false;
        }

        // Insert new entry into the appropriate half based on separator key
        if (Compare(comparator_, key, new_leaf->KeyAt(0)) < 0) {
            leaf->Insert(key, value, comparator_);
        } else {
            new_leaf->Insert(key, value, comparator_);
        }

        // 2. Push separator key (first key of new right leaf) up to parent
        KeyType separator_key = new_leaf->KeyAt(0);
        InsertIntoParent(leaf, separator_key, new_leaf);
        UpdateRecordCount(1);

        bpm_->UnpinPage(new_leaf->GetPageId(), true);
        bpm_->UnpinPage(leaf_frame->GetPageId(), true);
        return true;
    }

public:
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
    /**
     * @brief Create a new root page when the current root splits (root split).
     *
     * Handles both leaf root splits and internal root splits:
     * 1. Allocates a new internal root page via BufferPoolManager.
     * 2. Initializes the new root with INVALID_PAGE_ID parent and internal_max_size_.
     * 3. Populates it with (old_root->GetPageId(), separator_key, new_sibling->GetPageId()).
     * 4. Updates parent pointers on both old_root and new_sibling to point to the new root.
     * 5. Updates in-memory root_page_id_ and persistent BPlusTreeHeaderPage (new root_page_id, depth + 1).
     * 6. Unpins the new root page as dirty.
     *
     * @param old_root Pointer to the old root page (either leaf or internal).
     * @param separator_key The separator key between old_root and new_sibling.
     * @param new_sibling Pointer to the newly allocated sibling page.
     * @return page_id_t The page ID of the newly created root, or INVALID_PAGE_ID on failure.
     */
    page_id_t CreateNewRoot(BPlusTreePage *old_root, const KeyType &separator_key, BPlusTreePage *new_sibling) {
        if (old_root == nullptr || new_sibling == nullptr || bpm_ == nullptr) {
            return INVALID_PAGE_ID;
        }

        page_id_t new_root_pid = INVALID_PAGE_ID;
        Page *new_root_frame = bpm_->NewPage(&new_root_pid);
        if (new_root_frame == nullptr) {
            return INVALID_PAGE_ID;
        }

        auto *new_root = InternalPage::Cast(new_root_frame);
        new_root->Init(new_root_pid, INVALID_PAGE_ID, internal_max_size_);
        new_root->Populate(old_root->GetPageId(), separator_key, new_sibling->GetPageId());

        old_root->SetParentPageId(new_root_pid);
        new_sibling->SetParentPageId(new_root_pid);

        uint32_t new_depth = GetDepth() + 1;
        SetRootPageId(new_root_pid);
        UpdateHeaderMetadata(new_root_pid, new_depth, 0);

        bpm_->UnpinPage(new_root_pid, true);
        return new_root_pid;
    }

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
    LeafPage *Split(LeafPage *leaf) {
        if (leaf == nullptr || bpm_ == nullptr) {
            return nullptr;
        }

        page_id_t new_leaf_pid = INVALID_PAGE_ID;
        Page *new_leaf_frame = bpm_->NewPage(&new_leaf_pid);
        if (new_leaf_frame == nullptr) {
            return nullptr;
        }

        auto *new_leaf = LeafPage::Cast(new_leaf_frame);
        new_leaf->Init(new_leaf_pid, leaf->GetParentPageId(), leaf_max_size_);

        // Move upper half of entries and fix next-pointer chain:
        // leaf -> new_leaf -> old_next
        leaf->MoveHalfTo(new_leaf);

        return new_leaf;
    }

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
    InternalPage *Split(InternalPage *internal) {
        if (internal == nullptr || bpm_ == nullptr) {
            return nullptr;
        }

        page_id_t new_internal_pid = INVALID_PAGE_ID;
        Page *new_internal_frame = bpm_->NewPage(&new_internal_pid);
        if (new_internal_frame == nullptr) {
            return nullptr;
        }

        auto *new_internal = InternalPage::Cast(new_internal_frame);
        new_internal->Init(new_internal_pid, internal->GetParentPageId(), internal_max_size_);

        // Moves upper half of entries and reparents children via BPM
        internal->MoveHalfTo(new_internal, bpm_);

        return new_internal;
    }

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
                                page_id_t new_child_pid, KeyType *split_up_key) {
        if (parent == nullptr || bpm_ == nullptr || split_up_key == nullptr) {
            return nullptr;
        }

        page_id_t new_parent_pid = INVALID_PAGE_ID;
        Page *new_parent_frame = bpm_->NewPage(&new_parent_pid);
        if (new_parent_frame == nullptr) {
            return nullptr;
        }

        auto *new_parent = InternalPage::Cast(new_parent_frame);
        new_parent->Init(new_parent_pid, parent->GetParentPageId(), internal_max_size_);

        // Build temporary buffer containing existing entries + new entry in order
        std::vector<std::pair<KeyType, page_id_t>> entries;
        int old_idx = parent->ValueIndex(old_child_pid);
        for (int i = 0; i <= old_idx; ++i) {
            entries.push_back({parent->KeyAt(i), parent->ValueAt(i)});
        }
        entries.push_back({separator_key, new_child_pid});
        for (int i = old_idx + 1; i < parent->GetSize(); ++i) {
            entries.push_back({parent->KeyAt(i), parent->ValueAt(i)});
        }

        int total_entries = static_cast<int>(entries.size());
        int mid = total_entries / 2;
        *split_up_key = entries[mid].first;

        // Left internal node retains [0 .. mid - 1]
        parent->SetSize(mid);
        for (int i = 0; i < mid; ++i) {
            parent->SetKeyAt(i, entries[i].first);
            parent->SetValueAt(i, entries[i].second);
        }

        // Right internal node gets [mid .. total - 1]
        int new_size = total_entries - mid;
        new_parent->SetSize(new_size);
        new_parent->SetValueAt(0, entries[mid].second);
        for (int i = 1; i < new_size; ++i) {
            new_parent->SetKeyAt(i, entries[mid + i].first);
            new_parent->SetValueAt(i, entries[mid + i].second);
        }

        // Update parent pointers on all children moved to new_parent
        for (int i = 0; i < new_size; ++i) {
            page_id_t child_pid = new_parent->ValueAt(i);
            Page *child_frame = bpm_->FetchPage(child_pid);
            if (child_frame != nullptr) {
                BPlusTreePage::Cast(child_frame)->SetParentPageId(new_parent_pid);
                bpm_->UnpinPage(child_pid, true);
            }
        }

        // If new_child ended up in parent (left node), update its parent pointer to parent's ID
        if (parent->ValueIndex(new_child_pid) != -1) {
            Page *child_frame = bpm_->FetchPage(new_child_pid);
            if (child_frame != nullptr) {
                BPlusTreePage::Cast(child_frame)->SetParentPageId(parent->GetPageId());
                bpm_->UnpinPage(new_child_pid, true);
            }
        }

        return new_parent;
    }

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
    void InsertIntoParent(BPlusTreePage *old_child, const KeyType &separator_key, BPlusTreePage *new_child) {
        page_id_t parent_pid = old_child->GetParentPageId();

        if (parent_pid == INVALID_PAGE_ID) {
            // Root Split: old_child was root -> allocate new internal root and update root ID
            CreateNewRoot(old_child, separator_key, new_child);
            return;
        }

        Page *parent_frame = bpm_->FetchPage(parent_pid);
        if (parent_frame == nullptr) return;

        auto *parent = InternalPage::Cast(parent_frame);

        // Case 1: Parent has space
        if (parent->GetSize() < parent->GetMaxSize()) {
            parent->InsertNodeAfter(old_child->GetPageId(), separator_key, new_child->GetPageId());
            new_child->SetParentPageId(parent_pid);
            bpm_->UnpinPage(parent_pid, true);
            return;
        }

        // Case 2: Parent is full -> Split Internal Node!
        KeyType split_up_key{};
        InternalPage *new_parent = SplitInternal(parent, old_child->GetPageId(), separator_key,
                                                 new_child->GetPageId(), &split_up_key);
        if (new_parent == nullptr) {
            bpm_->UnpinPage(parent_pid, false);
            return;
        }

        page_id_t new_parent_pid = new_parent->GetPageId();

        // Recurse up to parent's parent
        InsertIntoParent(parent, split_up_key, new_parent);

        bpm_->UnpinPage(new_parent_pid, true);
        bpm_->UnpinPage(parent_pid, true);
    }

    /**
     * @brief Update persistent header metadata when tree root or record count changes.
     */
    void UpdateHeaderMetadata(page_id_t root_pid, uint32_t depth, int64_t record_count_delta) {
        if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
            Page *header_frame = bpm_->FetchPage(header_page_id_);
            if (header_frame != nullptr) {
                auto *header = BPlusTreeHeaderPage::Cast(header_frame);
                if (root_pid != INVALID_PAGE_ID) {
                    header->SetRootPageId(root_pid);
                }
                if (depth > 0) {
                    header->SetDepth(depth);
                }
                if (record_count_delta != 0) {
                    header->IncreaseRecordCount(record_count_delta);
                }
                bpm_->UnpinPage(header_page_id_, true);
            }
        }
    }

    void UpdateRecordCount(int64_t delta) {
        if (header_page_id_ != INVALID_PAGE_ID && bpm_ != nullptr) {
            Page *header_frame = bpm_->FetchPage(header_page_id_);
            if (header_frame != nullptr) {
                auto *header = BPlusTreeHeaderPage::Cast(header_frame);
                header->IncreaseRecordCount(delta);
                bpm_->UnpinPage(header_page_id_, true);
            }
        }
    }

public:
    // ==================== Tree Validation & Graphviz Diagnostics ====================

    /**
     * @brief Validate all B+ Tree structural, occupancy, sorting, and topological invariants.
     *
     * Invariants Verified:
     * 1. Root Invariants:
     *    - If tree is empty: root_page_id is INVALID_PAGE_ID, record_count is 0, depth is 0.
     *    - Root node has parent_page_id == INVALID_PAGE_ID.
     *    - If root is internal: size >= 2 and size <= internal_max_size_.
     * 2. Sortedness & Key Ordering Invariants:
     *    - Every Leaf node: keys are strictly ascending: key[i] < key[i+1].
     *    - Every Internal node: keys are strictly ascending: key[i] < key[i+1] for 1 <= i < size-1.
     *    - Subtree partitioning: for each child i of an internal node, all keys in child's subtree
     *      fall strictly within [lower_bound, upper_bound).
     * 3. Occupancy (Fill-Factor) Invariants:
     *    - Leaf nodes (non-root): size >= (leaf_max_size_ / 2) and size <= leaf_max_size_.
     *    - Internal nodes (non-root): size >= ((internal_max_size_ + 1) / 2) and size <= internal_max_size_.
     * 4. Parent Pointer Invariants:
     *    - For every child C of internal node P, C->GetParentPageId() == P->GetPageId().
     * 5. Leaf Chain (Sequential Linked List) Invariants:
     *    - Traversal along next_page_id pointers visits every leaf discovered from the tree hierarchy.
     *    - No cycles, no orphan/disconnected leaves.
     *    - Across adjacent leaves: max_key(leaf_A) <= min_key(leaf_B).
     *    - Sum of leaf keys equals total_keys and matches GetRecordCount().
     * 6. Depth Consistency:
     *    - Every leaf is at the exact same depth from root, matching GetDepth().
     * 7. Buffer Pool Pin Invariance:
     *    - Guarantees zero pin leaks: every page accessed is cleanly unpinned before returning.
     *
     * @return TreeValidationResult Result containing validity boolean, error description, and stats.
     */
    TreeValidationResult ValidateTree() const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);

        page_id_t root_pid = GetRootPageId();
        if (root_pid == INVALID_PAGE_ID) {
            if (GetRecordCount() != 0) {
                return {false, "Empty tree has non-zero record count: " + std::to_string(GetRecordCount())};
            }
            if (GetDepth() != 0) {
                return {false, "Empty tree has non-zero depth: " + std::to_string(GetDepth())};
            }
            return {true, "", 0, 0, 0, 0};
        }

        // 1. Validate Root Frame
        Page *root_frame = bpm_->FetchPage(root_pid);
        if (root_frame == nullptr) {
            return {false, "Failed to fetch root page frame " + std::to_string(root_pid)};
        }

        auto *root_page = BPlusTreePage::Cast(root_frame);
        if (root_page->GetParentPageId() != INVALID_PAGE_ID) {
            bpm_->UnpinPage(root_pid, false);
            return {false, "Root page has invalid parent_page_id: " + std::to_string(root_page->GetParentPageId())};
        }

        if (root_page->IsLeafPage()) {
            if (root_page->GetSize() < 1 || root_page->GetSize() > leaf_max_size_) {
                bpm_->UnpinPage(root_pid, false);
                return {false, "Root leaf page size out of bounds: " + std::to_string(root_page->GetSize())};
            }
        } else if (root_page->IsInternalPage()) {
            if (root_page->GetSize() < 2 || root_page->GetSize() > internal_max_size_) {
                bpm_->UnpinPage(root_pid, false);
                return {false, "Root internal page size out of bounds: " + std::to_string(root_page->GetSize())};
            }
        } else {
            bpm_->UnpinPage(root_pid, false);
            return {false, "Root page has invalid page type"};
        }
        bpm_->UnpinPage(root_pid, false);

        // 2. BFS Traversal to validate Sortedness, Occupancy, Parent Pointers, and Bounds
        struct BFSItem {
            page_id_t page_id;
            uint32_t depth;
            std::optional<KeyType> lower_bound;
            std::optional<KeyType> upper_bound;
        };

        std::queue<BFSItem> q;
        std::unordered_set<page_id_t> visited_pages;
        std::set<page_id_t> hierarchy_leaves;
        size_t hierarchy_key_count = 0;
        size_t leaf_page_count = 0;
        size_t internal_page_count = 0;
        uint32_t observed_leaf_depth = 0;

        q.push({root_pid, 1, std::nullopt, std::nullopt});
        visited_pages.insert(root_pid);

        while (!q.empty()) {
            BFSItem item = q.front();
            q.pop();

            Page *frame = bpm_->FetchPage(item.page_id);
            if (frame == nullptr) {
                return {false, "Failed to fetch page " + std::to_string(item.page_id) + " during validation"};
            }

            auto *page = BPlusTreePage::Cast(frame);

            if (page->IsLeafPage()) {
                auto *leaf = LeafPage::Cast(frame);
                leaf_page_count++;

                // Leaf depth consistency check
                if (observed_leaf_depth == 0) {
                    observed_leaf_depth = item.depth;
                } else if (observed_leaf_depth != item.depth) {
                    bpm_->UnpinPage(item.page_id, false);
                    return {false, "Leaf page " + std::to_string(item.page_id) + " at depth "
                                   + std::to_string(item.depth) + " but expected depth " + std::to_string(observed_leaf_depth)};
                }

                // Occupancy check
                if (item.page_id != root_pid) {
                    int min_leaf = leaf_max_size_ / 2;
                    if (leaf->GetSize() < min_leaf || leaf->GetSize() > leaf_max_size_) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Leaf page " + std::to_string(item.page_id) + " occupancy violation: size "
                                       + std::to_string(leaf->GetSize()) + " not in [" + std::to_string(min_leaf) + ", "
                                       + std::to_string(leaf_max_size_) + "]"};
                    }
                }

                // Sortedness check within leaf & subtree bounds check
                for (int i = 0; i < leaf->GetSize(); ++i) {
                    const KeyType &k = leaf->KeyAt(i);
                    if (item.lower_bound.has_value() && Compare(comparator_, k, *item.lower_bound) < 0) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Key in leaf " + std::to_string(item.page_id) + " violates lower bound"};
                    }
                    if (item.upper_bound.has_value() && Compare(comparator_, k, *item.upper_bound) >= 0) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Key in leaf " + std::to_string(item.page_id) + " violates upper bound"};
                    }
                    if (i > 0 && Compare(comparator_, leaf->KeyAt(i - 1), k) >= 0) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Leaf " + std::to_string(item.page_id) + " keys not strictly ascending"};
                    }
                }

                hierarchy_leaves.insert(item.page_id);
                hierarchy_key_count += leaf->GetSize();
                bpm_->UnpinPage(item.page_id, false);

            } else if (page->IsInternalPage()) {
                auto *internal = InternalPage::Cast(frame);
                internal_page_count++;

                // Occupancy check
                if (item.page_id != root_pid) {
                    int min_internal = (internal_max_size_ + 1) / 2;
                    if (internal->GetSize() < min_internal || internal->GetSize() > internal_max_size_) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Internal page " + std::to_string(item.page_id) + " occupancy violation: size "
                                       + std::to_string(internal->GetSize()) + " not in [" + std::to_string(min_internal) + ", "
                                       + std::to_string(internal_max_size_) + "]"};
                    }
                }

                // Sortedness check within internal node
                for (int i = 1; i < internal->GetSize(); ++i) {
                    const KeyType &k = internal->KeyAt(i);
                    if (item.lower_bound.has_value() && Compare(comparator_, k, *item.lower_bound) < 0) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Separator key in internal page " + std::to_string(item.page_id) + " violates lower bound"};
                    }
                    if (item.upper_bound.has_value() && Compare(comparator_, k, *item.upper_bound) >= 0) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Separator key in internal page " + std::to_string(item.page_id) + " violates upper bound"};
                    }
                    if (i > 1 && Compare(comparator_, internal->KeyAt(i - 1), k) >= 0) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Internal page " + std::to_string(item.page_id) + " keys not strictly ascending"};
                    }
                }

                // Validate parent pointers of all children
                for (int i = 0; i < internal->GetSize(); ++i) {
                    page_id_t child_pid = internal->ValueAt(i);
                    if (child_pid == INVALID_PAGE_ID) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Internal page " + std::to_string(item.page_id) + " has child pointer INVALID_PAGE_ID"};
                    }
                    if (visited_pages.count(child_pid)) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Cycle detected in tree: page " + std::to_string(child_pid) + " visited twice"};
                    }
                    visited_pages.insert(child_pid);

                    Page *child_frame = bpm_->FetchPage(child_pid);
                    if (child_frame == nullptr) {
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Failed to fetch child page " + std::to_string(child_pid)};
                    }
                    auto *child_page = BPlusTreePage::Cast(child_frame);
                    if (child_page->GetParentPageId() != item.page_id) {
                        page_id_t actual_parent = child_page->GetParentPageId();
                        bpm_->UnpinPage(child_pid, false);
                        bpm_->UnpinPage(item.page_id, false);
                        return {false, "Parent pointer mismatch: child " + std::to_string(child_pid)
                                       + " has parent " + std::to_string(actual_parent) + " but expected " + std::to_string(item.page_id)};
                    }
                    bpm_->UnpinPage(child_pid, false);

                    std::optional<KeyType> child_lower = (i == 0) ? item.lower_bound : std::make_optional(internal->KeyAt(i));
                    std::optional<KeyType> child_upper = (i == internal->GetSize() - 1) ? item.upper_bound : std::make_optional(internal->KeyAt(i + 1));
                    q.push({child_pid, item.depth + 1, child_lower, child_upper});
                }

                bpm_->UnpinPage(item.page_id, false);
            }
        }

        // 3. Validate Leaf Chain (Sequential Linked List)
        Page *leftmost_frame = FindLeafPage(KeyType{}, true);
        if (leftmost_frame == nullptr) {
            return {false, "FindLeafPage failed to find leftmost leaf page"};
        }
        page_id_t curr_leaf_pid = leftmost_frame->GetPageId();
        bpm_->UnpinPage(curr_leaf_pid, false);

        std::set<page_id_t> chain_leaves;
        std::optional<KeyType> prev_max_key;
        size_t chain_key_count = 0;

        while (curr_leaf_pid != INVALID_PAGE_ID) {
            if (chain_leaves.count(curr_leaf_pid)) {
                return {false, "Cycle detected in leaf next-pointer chain at page " + std::to_string(curr_leaf_pid)};
            }
            chain_leaves.insert(curr_leaf_pid);

            Page *leaf_frame = bpm_->FetchPage(curr_leaf_pid);
            if (leaf_frame == nullptr) {
                return {false, "Failed to fetch leaf page " + std::to_string(curr_leaf_pid) + " in chain"};
            }

            auto *leaf = LeafPage::Cast(leaf_frame);
            if (!leaf->IsLeafPage()) {
                bpm_->UnpinPage(curr_leaf_pid, false);
                return {false, "Page " + std::to_string(curr_leaf_pid) + " in leaf chain is not a leaf page"};
            }

            // Check key ordering across adjacent leaves
            if (leaf->GetSize() > 0) {
                if (prev_max_key.has_value() && Compare(comparator_, *prev_max_key, leaf->KeyAt(0)) >= 0) {
                    bpm_->UnpinPage(curr_leaf_pid, false);
                    return {false, "Key ordering violation across leaf boundary between previous leaf and leaf " + std::to_string(curr_leaf_pid)};
                }
                prev_max_key = leaf->KeyAt(leaf->GetSize() - 1);
            }

            chain_key_count += leaf->GetSize();
            page_id_t next_pid = leaf->GetNextPageId();
            bpm_->UnpinPage(curr_leaf_pid, false);
            curr_leaf_pid = next_pid;
        }

        // Cross-check hierarchy leaves vs chain leaves
        if (chain_leaves != hierarchy_leaves) {
            return {false, "Mismatch between leaves found via tree hierarchy (" + std::to_string(hierarchy_leaves.size())
                           + ") and leaf chain (" + std::to_string(chain_leaves.size()) + ")"};
        }

        // Cross-check record counts
        if (chain_key_count != hierarchy_key_count) {
            return {false, "Key count mismatch between hierarchy (" + std::to_string(hierarchy_key_count)
                           + ") and leaf chain (" + std::to_string(chain_key_count) + ")"};
        }

        if (chain_key_count != GetRecordCount()) {
            return {false, "Record count in header (" + std::to_string(GetRecordCount())
                           + ") does not match indexed keys (" + std::to_string(chain_key_count) + ")"};
        }

        // Depth check
        if (observed_leaf_depth != GetDepth()) {
            return {false, "Computed depth (" + std::to_string(observed_leaf_depth)
                           + ") does not match header depth (" + std::to_string(GetDepth()) + ")"};
        }

        return {true, "", chain_key_count, leaf_page_count, internal_page_count, observed_leaf_depth};
    }

    /**
     * @brief Convenience helper returning true if tree is valid, false otherwise.
     * @param error_out Optional string pointer to receive error description on failure.
     */
    bool CheckIntegrity(std::string *error_out = nullptr) const {
        auto res = ValidateTree();
        if (error_out != nullptr && !res.is_valid) {
            *error_out = res.error_message;
        }
        return res.is_valid;
    }

    /**
     * @brief Output the B+ Tree structure in Graphviz DOT format to an output stream.
     *
     * Nodes are rendered with distinct styles for internal and leaf nodes, showing page IDs,
     * types, sizes, parents, keys, values, and child pointer ports. Directed edges show the
     * hierarchical tree structure, and dashed green edges show the sequential leaf chain.
     *
     * @param out Output stream.
     */
    void ToDot(std::ostream &out) const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);

        page_id_t root_pid = GetRootPageId();
        if (root_pid == INVALID_PAGE_ID || bpm_ == nullptr) {
            out << "digraph BPlusTree {\n";
            out << "  labelloc=\"t\";\n";
            out << "  label=\"B+ Tree: " << name_ << " (Empty)\";\n";
            out << "}\n";
            return;
        }

        out << "digraph BPlusTree {\n";
        out << "  rankdir=TB;\n";
        out << "  node [shape=record, fontname=\"Courier\", fontsize=10];\n";
        out << "  edge [fontname=\"Courier\", fontsize=9];\n";
        out << "  labelloc=\"t\";\n";
        out << "  label=\"B+ Tree: " << name_ << " (Depth: " << GetDepth()
            << ", Records: " << GetRecordCount() << ", Root Page: " << root_pid << ")\";\n\n";

        std::queue<page_id_t> q;
        std::vector<page_id_t> leaf_pids;
        std::unordered_set<page_id_t> visited;

        q.push(root_pid);
        visited.insert(root_pid);

        while (!q.empty()) {
            page_id_t pid = q.front();
            q.pop();

            Page *frame = bpm_->FetchPage(pid);
            if (frame == nullptr) continue;

            auto *page = BPlusTreePage::Cast(frame);

            if (page->IsInternalPage()) {
                auto *internal = InternalPage::Cast(frame);

                out << "  page_" << pid << " [label=\"{Page " << pid
                    << " | Type: Internal | Size: " << internal->GetSize() << "/" << internal->GetMaxSize()
                    << " | Parent: " << internal->GetParentPageId() << "} | {";

                out << "<p0>";
                for (int i = 1; i < internal->GetSize(); ++i) {
                    out << " | " << FormatForDot(internal->KeyAt(i)) << " | <p" << i << ">";
                }
                out << "}\", style=filled, fillcolor=\"#E8EEF5\"];\n";

                for (int i = 0; i < internal->GetSize(); ++i) {
                    page_id_t child_pid = internal->ValueAt(i);
                    out << "  page_" << pid << ":p" << i << " -> page_" << child_pid << ";\n";
                    if (visited.insert(child_pid).second) {
                        q.push(child_pid);
                    }
                }
            } else if (page->IsLeafPage()) {
                auto *leaf = LeafPage::Cast(frame);
                leaf_pids.push_back(pid);

                out << "  page_" << pid << " [label=\"{Page " << pid
                    << " | Type: Leaf | Size: " << leaf->GetSize() << "/" << leaf->GetMaxSize()
                    << " | Parent: " << leaf->GetParentPageId()
                    << " | Next: " << leaf->GetNextPageId() << "} | {";

                for (int i = 0; i < leaf->GetSize(); ++i) {
                    if (i > 0) out << " | ";
                    out << FormatForDot(leaf->KeyAt(i)) << ": " << FormatForDot(leaf->ValueAt(i));
                }
                out << "}\", style=filled, fillcolor=\"#E8F5E9\"];\n";
            }

            bpm_->UnpinPage(pid, false);
        }

        out << "\n  // Sequential leaf chain edges\n";
        for (page_id_t leaf_pid : leaf_pids) {
            Page *frame = bpm_->FetchPage(leaf_pid);
            if (frame != nullptr) {
                auto *leaf = LeafPage::Cast(frame);
                page_id_t next_pid = leaf->GetNextPageId();
                if (next_pid != INVALID_PAGE_ID) {
                    out << "  page_" << leaf_pid << " -> page_" << next_pid
                        << " [style=dashed, color=\"#2E7D32\", constraint=false];\n";
                }
                bpm_->UnpinPage(leaf_pid, false);
            }
        }

        out << "}\n";
    }

    /**
     * @brief Return the Graphviz DOT representation as a std::string.
     */
    std::string ToDot() const {
        std::ostringstream oss;
        ToDot(oss);
        return oss.str();
    }

    /**
     * @brief Dump the Graphviz DOT representation to a file on disk.
     * @param filename Target file path.
     * @return true on success, false on I/O error.
     */
    bool DumpGraphviz(const std::string &filename) const {
        std::ofstream fout(filename);
        if (!fout.is_open()) return false;
        ToDot(fout);
        return fout.good();
    }

private:
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
