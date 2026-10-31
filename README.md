<p align="center">
  <img src="assets/dittodb_icon.png" alt="DittoDB Logo" width="180" />
</p>

<h1 align="center">DittoDB</h1>

<p align="center">
  <strong>A high-performance, malleable C++20 relational database storage engine.</strong><br>
  <em>Adaptive slotted pages • Scan-resistant LRU-K caching • Multi-page table heaps • Buffer-pool-backed extendible hash tables.</em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-blue.svg" alt="C++20" />
  <img src="https://img.shields.io/badge/Tests-255%2F255%20Passed-brightgreen.svg" alt="Tests" />
  <img src="https://img.shields.io/badge/Architecture-5--Tier%20Storage-purple.svg" alt="Architecture" />
  <img src="https://img.shields.io/badge/Sanitizers-TSan%20%7C%20Helgrind%20Clean-success.svg" alt="Sanitizers" />
  <img src="https://img.shields.io/badge/Build-CMake%20%7C%20Ninja-orange.svg" alt="Build" />
</p>

---

## Table of Contents
- [Architecture Overview](#architecture-overview)
- [Core Components & API Reference](#core-components--api-reference)
  - [1. Persistent Disk Storage (`DiskManager`)](#1-persistent-disk-storage-diskmanager)
  - [2. In-Memory Frames & Latches (`Page`)](#2-in-memory-frames--latches-page)
  - [3. Scan-Resistant Cache Eviction (`LRUKReplacer`)](#3-scan-resistant-cache-eviction-lrukreplacer)
  - [4. Buffer Pool Management (`BufferPoolManager`)](#4-buffer-pool-management-bufferpoolmanager)
  - [5. Slotted-Page Physical Storage (`TablePage`, `Tuple`, `RID`)](#5-slotted-page-physical-storage-tablepage-tuple-rid)
  - [6. Multi-Page Table Heap (`TableHeap`, `TableIterator`)](#6-multi-page-table-heap-tableheap-tableiterator)
  - [7. Buffer-Pool-Backed Hash Index (`ExtendibleHashTable`)](#7-buffer-pool-backed-hash-index-extendiblehashtable)
  - [8. B+ Tree Index Node Architecture (`BPlusTreePage`, `BPlusTreeInternalPage`, `BPlusTreeLeafPage`, `BPlusTreeHeaderPage`)](#8-b-tree-index-node-architecture-bplustreepage-bplustreeinternalpage-bplustreeleafpage-bplustreeheaderpage)
- [Key Engineering Trade-Offs](#key-engineering-trade-offs)
  - [Trade-off 1: Replacement Policy Selection (LRU vs. CLOCK vs. 2Q vs. LRU-K)](#trade-off-1-replacement-policy-selection-lru-vs-clock-vs-2q-vs-lru-k)
  - [Trade-off 2: Latching Granularity & Deadlock Avoidance Hierarchy](#trade-off-2-latching-granularity--deadlock-avoidance-hierarchy)
  - [Trade-off 3: Slotted Page Layout & Update Strategies (In-Place vs Migration)](#trade-off-3-slotted-page-layout--update-strategies-in-place-vs-migration)
- [Test Suite & Verification](#test-suite--verification)
  - [CMU 15-445 Project #1 Compliance](#cmu-15-445-project-1-compliance)
  - [Test Suite Coverage Breakdown (244/244 Passing)](#test-suite-coverage-breakdown-244244-passing)
  - [ThreadSanitizer & Helgrind Verification](#threadsanitizer--helgrind-verification)
- [Build & Run](#build--run)
- [Design Documents & Deep-Dives](#design-documents--deep-dives)

---

## Architecture Overview

DittoDB is built on a clean, decoupled 5-tier architecture where each storage layer strictly abstracts the complexity of the layers beneath it:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Tier 5: Tables, Hashes, & B+ Tree Indexing                                  │
│  - TableHeap: Doubly-linked chain of TablePages                             │
│  - TableIterator: C++20 standard-compliant forward iterator across pages    │
│  - ExtendibleHashTable: Buffer-pool-backed directory & bucket hash index    │
│  - B+ Tree Pages: HeaderPage root tracker, InternalPage, & LeafPage nodes   │
├─────────────────────────────────────────────────────────────────────────────┤
│ Tier 4: Records, Identifiers, & Slotted Pages                               │
│  - TablePage: Dual-end growth slotted-page layout on 4096-byte raw Page     │
│  - RID: Packed 64-bit logical record address (page_id, slot_num)            │
│  - Tombstones & Slot Reuse: High-bit soft delete & online defragmentation   │
│  - Tuple: Variable-length binary record payload abstraction                 │
├─────────────────────────────────────────────────────────────────────────────┤
│ Tier 3: Buffer Pool Management & Frame Caching                              │
│  - BufferPoolManager: Fixed-size cache of memory frames (page_id ◄─► frame) │
│  - Pin / Unpin protocol with sticky dirty tracking & eviction sync          │
│  - Zero-copy memory transmutation for high-level page types                 │
├──────────────────────────────────────┬──────────────────────────────────────┤
│ Tier 2A: Replacement Policy          │ Tier 2B: In-Memory Page Frames       │
│  - LRUKReplacer (K=2)                │  - Page: 4096-byte aligned frame     │
│  - Cold queue (FIFO for < K accesses)│  - std::shared_mutex R/W latch       │
│  - Hot queue (Backward K-distance)   │  - Atomic pin counter & dirty flag   │
├──────────────────────────────────────┴──────────────────────────────────────┤
│ Tier 1: Persistent Disk Storage                                             │
│  - DiskManager: Page-addressed 4096-byte block file I/O                     │
│  - Monotonic page allocation, zero-padded sparse writes, integrity checks   │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Core Components & API Reference

### 1. Persistent Disk Storage (`DiskManager`)
- **Headers:** [`include/disk_manager.h`](include/disk_manager.h) · **Source:** [`src/disk_manager.cpp`](src/disk_manager.cpp)
- **Design Deep-Dive:** [`docs/disk_manager_design.md`](docs/disk_manager_design.md)

Manages raw file I/O in fixed 4096-byte blocks (`PAGE_SIZE`). Enforces exact block alignment on the database file, transparently zero-fills sparse page allocations to prevent undefined reads from unwritten blocks, and protects I/O streams with an internal mutex.

```cpp
void WritePage(page_id_t page_id, const char *page_data);
void ReadPage(page_id_t page_id, char *page_data);
page_id_t AllocatePage();
size_t GetFileSize() const;
size_t GetNumPages() const;
void Flush();
```

---

### 2. In-Memory Frames & Latches (`Page`)
- **Headers:** [`include/page.h`](include/page.h) · **Source:** [`src/page.cpp`](src/page.cpp)

Represents a 4096-byte physical memory slot inside the buffer pool. Contains an atomic pin counter, atomic dirty bit, and a reader-writer latch (`std::shared_mutex`) enabling concurrent readers (`RLatch()`) or exclusive writers (`WLatch()`).

```cpp
char *GetData();
page_id_t GetPageId() const;
int GetPinCount() const;
bool IsDirty() const;

void WLatch();   // Exclusive write latch
void WUnlatch();
void RLatch();   // Shared read latch
void RUnlatch();
```

---

### 3. Scan-Resistant Cache Eviction (`LRUKReplacer`)
- **Headers:** [`include/lru_k_replacer.h`](include/lru_k_replacer.h) · **Source:** [`src/lru_k_replacer.cpp`](src/lru_k_replacer.cpp)
- **Design Deep-Dive:** [`docs/lru_k_design.md`](docs/lru_k_design.md)

Implements the **LRU-K** page replacement algorithm (O'Neil et al., 1993) with default $K=2$. Prevents sequential table scans from flushing hot index and metadata pages from the buffer pool.

```cpp
bool Evict(frame_id_t *frame_id);
void RecordAccess(frame_id_t frame_id);
void SetEvictable(frame_id_t frame_id, bool set_evictable);
void Remove(frame_id_t frame_id);
size_t Size() const;
```

#### Queue Transition Dynamics:
```
           [Frame Access Recorded]
                      │
                      ▼
             Access Count < K ?
             ├── YES ──► Cold Queue (FIFO eviction, +inf backward distance)
             └── NO  ──► Hot Queue (Evicted by earliest K-th backward timestamp)
```

---

### 4. Buffer Pool Management (`BufferPoolManager`)
- **Headers:** [`include/buffer_pool_manager.h`](include/buffer_pool_manager.h) · **Source:** [`src/buffer_pool_manager.cpp`](src/buffer_pool_manager.cpp)

Maintains an in-memory pool of mapped pages using a hash table (`page_table_: page_id -> frame_id`). Coordinates pin/unpin reference counts, coordinates dirty page writebacks with `DiskManager`, and orchestrates victim frame eviction via `LRUKReplacer`.

```cpp
Page *FetchPage(page_id_t page_id);
bool UnpinPage(page_id_t page_id, bool is_dirty);
Page *NewPage(page_id_t *page_id);
bool DeletePage(page_id_t page_id);
bool FlushPage(page_id_t page_id);
void FlushAllPages();
```

---

### 5. Slotted-Page Physical Storage (`TablePage`, `Tuple`, `RID`)
- **Headers:** [`include/table_page.h`](include/table_page.h) · [`include/tuple.h`](include/tuple.h) · [`include/rid.h`](include/rid.h)
- **Source:** [`src/table_page.cpp`](src/table_page.cpp) · [`src/tuple.cpp`](src/tuple.cpp)
- **Design Deep-Dive:** [`docs/slotted_page_design.md`](docs/slotted_page_design.md)

Implements the classic PostgreSQL/SQLite slotted-page architecture directly on top of raw 4096-byte `Page` buffers:

```
Offset 0                                                              Offset 4096
┌─────────────────┬──────────────────┬──────────────┬──────────────┬─────────────┐
│ Page Header     │ Slot Directory   │ Free Space   │ Tuple 1 Data │ Tuple 0 Data│
│ (24 bytes)      │ [S0][S1][S2]...  │ (Contiguous) │ (Variable)   │ (Variable)  │
└─────────────────┴──────────────────┴──────────────┴──────────────┴─────────────┘
▲                 ▲                  ▲              ▲                            ▲
0                 24                 Lower (pd)     Upper (free_ptr)             4096
                                  (grows ──►)       (◄── grows)
```

- **Dual-End Growth:** Slot headers grow forward from byte 24; tuple payloads grow backward from byte 4096.
- **Tombstones & Reversible Deletion:** `MarkDelete(rid)` sets the 31st bit (`1u << 31`) on the slot size. This achieves an $O(1)$ soft delete without memory relocation, permitting instant rollback via `RollbackDelete(rid)`.
- **Slot Reuse:** `ApplyDelete(rid)` zeroes the slot, making it eligible for subsequent inserts without extending the directory.
- **Defragmentation (`Defragment()`):** Compresses live tuples against the high memory boundary, eliminating fragmentation holes while **preserving identical slot numbers (RID stability)**.
- **`TablePageIterator`:** Implements `std::forward_iterator` over live tuples on a single page, transparently skipping empty and tombstoned slots.

---

### 6. Multi-Page Table Heap (`TableHeap`, `TableIterator`)
- **Headers:** [`include/table_heap.h`](include/table_heap.h) · **Source:** [`src/table_heap.cpp`](src/table_heap.cpp)

Represents a physical table as a doubly-linked list of `TablePage`s (`prev_page_id`, `next_page_id`):

```
Page 0 (TablePage)               Page 1 (TablePage)               Page 2 (TablePage)
┌──────────────────────┐         ┌──────────────────────┐         ┌──────────────────────┐
│ Prev: -1  | Next: 1  │ ◄─────► │ Prev: 0   | Next: 2  │ ◄─────► │ Prev: 1   | Next: -1 │
│ [Slot 0] [Slot 1]... │         │ [Slot 0] [Slot 1]... │         │ [Slot 0] [Slot 1]... │
└──────────────────────┘         └──────────────────────┘         └──────────────────────┘
```

- **Multi-Page Spanning Inserts:** Automatically searches the page chain for space; allocates and links new pages when necessary.
- **Cross-Page Migration Updates:** If an updated tuple grows and cannot fit on its current page even after compaction:
  1. Soft-deletes the old tuple on the current page (`MarkDelete(old_rid)`).
  2. Inserts the updated tuple on another page in the heap.
  3. Returns the `new_rid` for secondary index pointer updates.
  4. Automatically rolls back the delete if the insertion fails, guaranteeing atomicity.
- **`TableIterator`:** A global table scan forward iterator that advances across page boundaries, fetches/unpins buffer pool frames, and skips tombstoned records.

---

### 7. Buffer-Pool-Backed Hash Index (`ExtendibleHashTable`)
- **Headers:** [`include/extendible_hash_table.h`](include/extendible_hash_table.h) · [`include/extendible_htable_directory_page.h`](include/extendible_htable_directory_page.h) · [`include/extendible_htable_bucket_page.h`](include/extendible_htable_bucket_page.h)
- **Source:** [`src/extendible_hash_table.cpp`](src/extendible_hash_table.cpp)

A disk-backed, buffer-pool-managed dynamic hash index:
- **Directory Page:** Tracks global depth ($D$), local depths ($d_i$), and bucket page IDs.
- **Bucket Pages:** Fixed-capacity array of key-value pairs with atomic insert, lookup, and removal.
- **Dynamic Doubling & Splits:** When a bucket overflows, directory capacity doubles ($2^D \to 2^{D+1}$) without moving existing pages. The bucket splits, local depth increments, and entries redistribute across the split image.

---

### 8. B+ Tree Index Architecture (`BPlusTree`, `BPlusTreePage`, `BPlusTreeInternalPage`, `BPlusTreeLeafPage`, `BPlusTreeHeaderPage`)
- **Headers:** [`include/b_plus_tree.h`](include/b_plus_tree.h) · [`include/common_page_header.h`](include/common_page_header.h) · [`include/b_plus_tree_page.h`](include/b_plus_tree_page.h) · [`include/b_plus_tree_internal_page.h`](include/b_plus_tree_internal_page.h) · [`include/b_plus_tree_leaf_page.h`](include/b_plus_tree_leaf_page.h) · [`include/b_plus_tree_header_page.h`](include/b_plus_tree_header_page.h) · [`include/generic_key.h`](include/generic_key.h)

DittoDB features an in-place B+ Tree index storage layer built directly on top of 4096-byte `Page` memory frames without dynamic heap allocations inside node blocks. All node types inherit from a shared 24-byte header enabling polymorphic type inspection, WAL logging, and zero-copy reinterpret casting:

#### 1. Common Page Header Layout (24 Bytes)
Shared binary header present at byte offset 0 across all B+ Tree node types:

```
Offset 0                                                                       Offset 24
┌───────────────┬───────────────┬───────────────┬───────────────┬────────────────┬───────────────┐
│ page_type_    │ lsn_          │ size_         │ max_size_     │ parent_page_id │ page_id_      │
│ (4 Bytes)     │ (4 Bytes)     │ (4 Bytes)     │ (4 Bytes)     │ (4 Bytes)      │ (4 Bytes)     │
└───────────────┴───────────────┴───────────────┴───────────────┴────────────────┴───────────────┘
▲               ▲               ▲               ▲               ▲                ▲               ▲
0               4               8               12              16               20              24
```
- **`page_type_` (4B):** Enum identifying `BPLUS_TREE_INTERNAL_PAGE` (2), `BPLUS_TREE_LEAF_PAGE` (3), or `BPLUS_TREE_HEADER_PAGE` (4).
- **`lsn_` (4B):** Log Sequence Number reserved for Write-Ahead Logging (WAL) recovery.
- **`size_` (4B):** Number of active valid entries currently stored in the node.
- **`max_size_` (4B):** Maximum capacity before triggering node split ($N$).
- **`parent_page_id_` (4B):** Pointer to parent internal node (`INVALID_PAGE_ID` for root).
- **`page_id_` (4B):** Physical disk block identifier of this page.

---

#### 2. B+ Tree Internal Page Layout (Routing Node)
Internal nodes guide tree traversal down to the target leaf. Stored as an array of `(Key, child_page_id_t)` pairs:

```
Offset 0                                                                               Offset 4096
┌──────────────────────┬─────────────┬─────────────┬─────────────┬─────┬─────────────────────────┐
│ CommonPageHeader     │ [Dummy, P0] │  [Key1, P1] │  [Key2, P2] │ ... │ [Key(N-1), P(N-1)]      │
│ (24 Bytes)           │ Child 0 ptr │ Key1 | ptr1 │ Key2 | ptr2 │ ... │ Key(N-1) | ptr(N-1)     │
└──────────────────────┴─────────────┴─────────────┴─────────────┴─────┴─────────────────────────┘
▲                      ▲                                                                         ▲
0                      24 (Entry Array `array_[]`)                                               4096

Child Routing Invariant:
       keys < Key1     ──► Follow Child P0
Key1 ≤ keys < Key2     ──► Follow Child P1
Key2 ≤ keys < Key3     ──► Follow Child P2
       keys ≥ Key(N-1) ──► Follow Child P(N-1)
```
- **Dummy First Key:** `array_[0].first` is unmapped/ignored; `array_[0].second` holds pointer `P0` for all keys smaller than `Key1`.
- **Fast Binary Search:** `Lookup(key)` performs binary search over `array_[1 .. size-1]` to determine the routing child pointer in $O(\log N)$ time.
- **Insert-After Helper:** `InsertNodeAfter(old_value, new_key, new_value)` seamlessly splits and links new sibling pointers during child splits.

---

#### 3. B+ Tree Leaf Page Layout (Data Node)
Leaf nodes store the indexed key-value payload `(Key, RID)` and form a singly-linked sequential scan chain:

```
Offset 0                                                                               Offset 4096
┌──────────────────────┬──────────────┬─────────────┬─────────────┬─────────────┬───┬────────────┐
│ CommonPageHeader     │ next_page_id │  [Key0, R0] │  [Key1, R1] │  [Key2, R2] │...│[Key(M), RM]│
│ (24 Bytes)           │ (4B) + Pad4B │ Sorted pair │ Sorted pair │ Sorted pair │...│Sorted pair │
└──────────────────────┴──────────────┴─────────────┴─────────────┴─────────────┴───┴────────────┘
▲                      ▲              ▲                                                          ▲
0                      24             32 (Aligned Entry Array `array_[]`)                        4096
                                      │
                                      ▼
             Sequential Chain: Leaf Page ◄──next_page_id──► Next Leaf Page
```
- **Natural Alignment:** 4-byte padding follows `next_page_id_` so the entry array aligns cleanly on 8-byte boundaries for 64-bit keys and RIDs.
- **Strict Key Ordering:** Entries are maintained in sorted order: $\text{Key}_0 < \text{Key}_1 < \dots < \text{Key}_{M-1}$.
- **Linked Leaf Chain:** `next_page_id_` enables $O(1)$ page-to-page sequential iterator traversal across ranges without re-traversing internal nodes from the root.
- **Sorted Insert & Search:** Binary search `KeyIndex()` and `Lookup()` execute in $O(\log M)$; `Insert()` shifts entries in-place and rejects duplicates.

---

#### 4. B+ Tree Header Page Layout (Root Persistence)
A dedicated root tracking page that anchors the index on persistent storage:

```
Offset 0                                                                               Offset 4096
┌──────────────────┬──────────────┬─────────────┬──────────────┬──────────┬──────────┬───────────┐
│ CommonPageHeader │ root_page_id │ tree_depth_ │ record_count │ key_size │ val_size │ Reserved  │
│ (24 Bytes)       │ (4 Bytes)    │ (4 Bytes)   │ (8 Bytes)    │ (4 Bytes)│ (4 Bytes)│ (4048 B)  │
└──────────────────┴──────────────┴─────────────┴──────────────┴──────────┴──────────┴───────────┘
▲                  ▲              ▲             ▲              ▲          ▲          ▲           ▲
0                  24             28            32             40         44         48          4096
```
- **Root Tracking:** `root_page_id_` reliably stores the current root page ID (updated atomically during root splits and merges, or `INVALID_PAGE_ID` when empty).
- **Restart Recovery:** Persisted via `BufferPoolManager` and `DiskManager` so index roots are accurately restored across engine crashes and restarts.
- **Metadata Diagnostics:** Records `tree_depth_`, total indexed `record_count_`, and key/value sizes for schema validation.

---

#### 5. Complete Index Structure & Navigation Traversal

```
                               ┌────────────────────────────────────────┐
                               │       Internal Node (Root: Page 10)    │
                               │  Header: Type=Internal, Size=3, Ptr=-1 │
                               │   [*, P1]   |   [50, P2]  |  [100, P3] │
                               └───────┬────────────┬────────────┬──────┘
                                       │            │            │
                    ┌──────────────────┘            │            └──────────────────┐
                    ▼                               ▼                               ▼
       ┌────────────────────────┐      ┌────────────────────────┐      ┌────────────────────────┐
       │   Leaf Node (Page 1)   │      │   Leaf Node (Page 2)   │      │   Leaf Node (Page 3)   │
       │ NextPageId = 2         │─────►│ NextPageId = 3         │─────►│ NextPageId = -1        │
       │ (10, R1)  |  (30, R2)  │      │ (50, R3)  |  (75, R4)  │      │ (100, R5) | (200, R6)  │
       └────────────────────────┘      └────────────────────────┘      └────────────────────────┘
       ◄─────────────────────────── Sequential Leaf Chain ($O(1)$ Range Scans) ─────────────────►
```

#### 6. Point Lookup (`GetValue`) & Strict Page Unpin Protocol
Point lookup locates the exact record associated with a key:
- **Hierarchical Descent (`FindLeafPage`):** Starting at `GetRootPageId()`, the engine fetches the root page and checks `IsLeafPage()`. If internal, it performs binary search on `array_[1 .. size-1]` to identify the routing child pointer, fetches the child page frame, and **immediately unpins the parent internal page frame** (`bpm_->UnpinPage(curr_pid, false)`).
- **Leaf Binary Search:** On reaching the leaf node, `leaf->Lookup(key, result, comparator_)` runs binary search in $O(\log M)$ time across the sorted `(key, RID)` entries.
- **Strict Zero-Leak Unpinning Guarantee:** Every page accessed during traversal (the persistent `BPlusTreeHeaderPage`, all traversed internal nodes, and the final leaf node) is guaranteed to be unpinned with `is_dirty = false` before `GetValue` returns.

---

## Key Engineering Trade-Offs

### Trade-off 1: Replacement Policy Selection (LRU vs. CLOCK vs. 2Q vs. LRU-K)

| Policy | Scan Resistance | CPU Overhead | Memory Overhead | Implementation Complexity |
| :--- | :---: | :---: | :---: | :---: |
| **Standard LRU** | ❌ None | $O(1)$ | Low (doubly-linked list) | Low |
| **CLOCK (Second Chance)** | ❌ None | $O(1)$ | Minimal ($1$ reference bit) | Very Low |
| **2Q (Johnson & Shasha)** | ⚠️ Partial | $O(1)$ | Medium ($2$ distinct queues) | Medium |
| **LRU-K (O'Neil et al., 1993)** | ✅ **Strong (Mathematical Guarantee)** | **$O(\log N)$ / $O(N)$** | **Medium ($K$ timestamps/node)** | **Moderate** |

#### Why LRU-K ($K=2$) was chosen for DittoDB:
1. **The Sequential Scan Problem:** Relational databases routinely execute full table scans (`SELECT * FROM table`). Under standard LRU or CLOCK, a scan over a table larger than the buffer pool completely purges all hot index pages, B+Tree roots, and directory pages from cache.
2. **Mathematical Protection:** In LRU-K, a page touched only once during a table scan receives a backward $K$-distance of $+\infty$ and is placed in the **Cold Queue**. Pages in the cold queue are evicted first via FIFO order. They never enter the **Hot Queue**, guaranteeing that frequently queried pages are preserved.
3. **Overhead Justification:** For $K=2$, storing two timestamps per frame requires only 16 bytes of metadata per frame. In modern hardware, trading 16 bytes per frame for complete immunity to cache pollution from sequential scans is an overwhelmingly favorable trade-off.

---

### Trade-off 2: Latching Granularity & Deadlock Avoidance Hierarchy

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ BufferPoolManager Global Latch (std::mutex bpm_latch_)                      │
│ - Scope: Page Table, Free List, Replacer metadata                           │
│ - Hold Duration: Very short (< 1 µs)                                        │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │ Protects frame assignment
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ Page Frame Latches (std::shared_mutex rwlatch_)                             │
│ - Scope: 4096-byte memory payload access                                    │
│ - Hold Duration: Length of tuple read/write operations                      │
│ - Multiple Readers: RLatch() (Shared)                                       │
│ - Single Writer:    WLatch() (Exclusive)                                    │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### Why not a single global lock or purely lock-free frames?
1. **Coarse-Grained BPM Latch vs Fine-Grained Page Latches:** If a single mutex guarded the entire buffer pool including page reads/writes, concurrency would drop to zero; only one thread in the entire DBMS could read a page at a time.
2. **Decoupled Architecture:** DittoDB uses a coarse mutex *only* for buffer pool metadata updates (`FetchPage`, `UnpinPage`, `NewPage`). Once a page pointer is retrieved and pinned, the worker thread **drops the BPM mutex** and acquires the page's individual `std::shared_mutex` (`RLatch` or `WLatch`). This allows unbounded concurrent readers on separate pages.
3. **Deadlock Prevention Hierarchy:**
   $$\mathbf{BPM\ Mutex} \longrightarrow \mathbf{Page\ Latch}$$
   - Threads must **never** call buffer pool methods while holding a page latch.
   - Pin counts and dirty flags use `std::atomic<int>` and `std::atomic<bool>`. Calling `UnpinPage()` does not require holding the page's exclusive data latch, completely eliminating lock-inversion deadlocks.

---

### Trade-off 3: Slotted Page Layout & Update Strategies (In-Place vs Migration)

When a tuple update causes its size to expand beyond the available space in its current slot:

1. **Option A: Forwarding Pointers (PostgreSQL approach)**
   - *Pros:* Preserves original RID.
   - *Cons:* Creates pointer chains (chasing multiple disk pages for a single tuple read); requires complex background vacuuming.
2. **Option B: Immediate Heap-Wide Compaction**
   - *Pros:* No forwarding pointers.
   - *Cons:* Extremely expensive per update; locks multiple pages simultaneously.
3. **Option C: Two-Tier Strategy (In-Place Compaction + Cross-Page Migration) — Chosen:**
   - **Tier 1 (Local):** If the updated tuple fits after local `Defragment()`, compaction shifts live records toward byte 4096, keeping the exact same RID with zero index modification.
   - **Tier 2 (Cross-Page Migration):** If the tuple cannot fit on the current page even after compaction, DittoDB marks the old slot as tombstoned, inserts the updated tuple on a new page, and returns `new_rid`. An automatic rollback mechanism reverts the deletion if the insert fails, guaranteeing transactional consistency.

---

## Test Suite & Verification

### CMU 15-445 Project #1 Compliance
The DittoDB test suite covers all requirements and canonical edge cases defined in CMU 15-445 (Buffer Pool Manager):

- **LRU-K Replacer:** Canonical sample test (frames 1..5), FIFO tie-breaking for frames with $< K$ accesses, post-eviction history wiping, idempotent `SetEvictable` calls, dynamic cold-to-hot queue migration.
- **Buffer Pool Manager:** Canonical 10-page cycle, full 4096-byte randomized binary payloads with embedded null bytes (`\0`), `NewPage(nullptr)` parameter safety, sticky dirty bit persistence across interleaved unpins, unpinned dirty page deletion, and destructor flush persistence.

### Test Suite Coverage Breakdown (251/251 Passing)

| Test Suite | Tests | Features Covered |
| :--- | :---: | :--- |
| `DiskManagerTest` | 42 | Block alignment, monotonic allocation, sparse zero-filling, persistence, corruption detection, concurrent I/O stress |
| `PageTest` | 28 | Frame payload integrity, atomic pin counters, dirty flags, shared/exclusive latches, concurrent reader/writer stress |
| `LRUKReplacerTest` | 19 | Cold/hot queue transitions, $K$-distance eviction order, CMU 15-445 FIFO tie-breakers, history wiping, 100k-op benchmark |
| `BufferPoolManagerTest` | 20 | Page table caching, sticky dirty flag persistence, binary safety, destructor auto-flush, pin protection |
| `BufferPoolIntegrationTest` | 15 | Mixed database workloads, repeated access patterns, sequential scans, frame-to-page mappings |
| `RIDTest` | 9 | 64-bit int packing, equality, ordering, stream formatting, `std::hash` in `std::unordered_map` |
| `TablePageTest` | 41 | Dual-end growth, variable-length inserts, in-place updates, tombstones, defragmentation, `TablePageIterator`, RID stability |
| `TableHeapTest` | 9 | Linked-page creation, multi-page spanning inserts, cross-page migration update, `TableIterator` across pages, crash persistence |
| `ExtendibleHashTableTest` | 10 | Directory page doubling/shrinking, split images, hash masking, bucket splits, key redistribution, persistence across eviction |
| `EndToEndStorageIntegrationTest` | 4 | Real DiskManager + LRUK + BPM + TablePage + TableHeap cooperating across 50+ pages with 4-5 frames, forced dirty-evictions, crash recovery |
| `BufferPoolManagerConcurrentTest` | 7 | Concurrent `FetchPage`/`UnpinPage` contention, 10-thread eviction storm, concurrent `NewPage` allocation, reader-writer latching isolation, pin exhaustion recovery, frame reservation on unbuffered page |
| `CommonPageHeaderTest` | 7 | 24-byte layout verification, natural 4-byte word alignment, sentinel initialization, in-place raw memory casting, Page/TablePage integration, BPlusTreePage casting |
| `BPlusTreeInternalPageTest` | 9 | (key, child_page_id) array layout, binary-search child lookup, insert-after helper, root split populate, removal shift, zero-copy Page transmutation, custom comparators |
| `BPlusTreeLeafPageTest` | 10 | Sorted (key, RID) array layout, next_page_id sequential linking, binary search lookup, sorted insert, duplicate rejection, split/merge helpers, custom comparators |
| `GenericKeyTest` | 9 | GenericKey<N> layout, order-preserving integer/float/string serialization, memcmp-ready lexicographical encoding, GenericComparator, BPlusTreePageSizeEstimator capacity calculations, leaf/internal integration |
| `BPlusTreeHeaderPageTest` | 5 | 4096-byte header layout, root page pointer persistence, crash/restart durability via BPM/DiskManager, tree depth & record accounting, isolated leaf & internal node transitions |
| `BPlusTreeTest` | 11 | Empty tree insert, leaf with space insert, duplicate key rejection, leaf split with next-pointer chain & separator push-up, root-to-leaf traversal, binary search GetValue, zero pin leaks, multi-level routing, GenericKey, persistence |

```
100% tests passed, 0 tests failed out of 255
Total Test time (real) = 13.62 sec
```

### ThreadSanitizer & Helgrind Verification

DittoDB is verified for zero data races, deadlocks, or lock-order inversions:

```bash
# 1. ThreadSanitizer (TSan) - Zero race conditions reported
./scripts/run_tsan.sh

# 2. Valgrind Helgrind - Clean POSIX pthread synchronization
./scripts/run_helgrind.sh
```

---

## Build & Run

### Requirements
- C++20 compliant compiler (`GCC 10+`, `Clang 12+`, `MSVC 2022`)
- CMake $\ge 3.20$
- Ninja build system
- Google Test

### Build Commands
```bash
# Configure with Ninja and C++20
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build all libraries, executables, and test suites
cmake --build build

# Run all 203 unit and integration tests
ctest --test-dir build --output-on-failure
```

### Run Specific Test Binaries
```bash
./build/buffer_pool_manager_test.exe                     # BPM unit & CMU 15-445 edge cases
./build/lru_k_replacer_test.exe                         # LRU-K replacer & queue migration tests
./build/table_page_test.exe                             # Slotted page & defragmentation tests
./build/table_heap_test.exe                             # Multi-page TableHeap & migration tests
./build/extendible_hash_table_test.exe                  # Extendible hash directory & bucket splits
./build/e2e_storage_test.exe                            # Multi-tier storage integration tests
./build/buffer_pool_manager_concurrent_test.exe         # Multithreaded stress & latch concurrency
```

---

## Design Documents & Deep-Dives

- 🗄️ [Slotted Page Storage Architecture & Postgres Comparison](docs/slotted_page_design.md)
- 🧠 [LRU-K Replacer Design & Mathematical Formulation](docs/lru_k_design.md)
- 💾 [Disk Manager Storage & Block Alignment](docs/disk_manager_design.md)

---

<p align="center">
  <strong>DittoDB</strong> • Crafted with precision as a high-performance database storage engine portfolio piece.
</p>
