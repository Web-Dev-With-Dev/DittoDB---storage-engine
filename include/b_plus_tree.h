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
    LeafPage *Split(LeafPage *leaf) {
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
     * @brief Push the separator key and new child pointer up to the parent internal node.
     * - If old_child was root: allocates a new internal root, populates it, and increments tree depth.
     * - If parent has space: inserts separator key and new child pointer using InsertNodeAfter.
     * - If parent is full: splits the parent internal node and recursively pushes up the median separator.
     *
     * @param old_child Pointer to left child page.
     * @param separator_key Key separating old_child and new_child (first key of new_child for leaves).
     * @param new_child Pointer to newly created right child page.
     */
    void InsertIntoParent(BPlusTreePage *old_child, const KeyType &separator_key, BPlusTreePage *new_child) {
        page_id_t parent_pid = old_child->GetParentPageId();

        if (parent_pid == INVALID_PAGE_ID) {
            // Root Split: old_child was root -> allocate new internal root
            page_id_t new_root_pid = INVALID_PAGE_ID;
            Page *new_root_frame = bpm_->NewPage(&new_root_pid);
            if (new_root_frame == nullptr) return;

            auto *new_root = InternalPage::Cast(new_root_frame);
            new_root->Init(new_root_pid, INVALID_PAGE_ID, internal_max_size_);
            new_root->Populate(old_child->GetPageId(), separator_key, new_child->GetPageId());

            old_child->SetParentPageId(new_root_pid);
            new_child->SetParentPageId(new_root_pid);

            SetRootPageId(new_root_pid);
            UpdateHeaderMetadata(new_root_pid, GetDepth() + 1, 0);

            bpm_->UnpinPage(new_root_pid, true);
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
        page_id_t new_parent_pid = INVALID_PAGE_ID;
        Page *new_parent_frame = bpm_->NewPage(&new_parent_pid);
        if (new_parent_frame == nullptr) {
            bpm_->UnpinPage(parent_pid, false);
            return;
        }

        auto *new_parent = InternalPage::Cast(new_parent_frame);
        new_parent->Init(new_parent_pid, parent->GetParentPageId(), internal_max_size_);

        // Build temporary buffer containing existing entries + new entry in order
        std::vector<std::pair<KeyType, page_id_t>> entries;
        int old_idx = parent->ValueIndex(old_child->GetPageId());
        for (int i = 0; i <= old_idx; ++i) {
            entries.push_back({parent->KeyAt(i), parent->ValueAt(i)});
        }
        entries.push_back({separator_key, new_child->GetPageId()});
        for (int i = old_idx + 1; i < parent->GetSize(); ++i) {
            entries.push_back({parent->KeyAt(i), parent->ValueAt(i)});
        }

        int total_entries = static_cast<int>(entries.size());
        int mid = total_entries / 2;
        KeyType split_up_key = entries[mid].first;

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

    std::string name_;
    BufferPoolManager *bpm_{nullptr};
    page_id_t header_page_id_{INVALID_PAGE_ID};
    page_id_t root_page_id_{INVALID_PAGE_ID};
    KeyComparator comparator_{};
    int leaf_max_size_{0};
    int internal_max_size_{0};
    mutable std::unique_ptr<std::shared_mutex> rwlock_{std::make_unique<std::shared_mutex>()};
};
