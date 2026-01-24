# Race Condition Fixes Tracker

This document tracks specific race conditions identified in the Cottage kernel and their fix status.

## Status Legend
- [ ] Not started
- [~] In progress
- [x] Fixed
- [-] Won't fix (with reason)

---

## Critical Priority

### VFS (Virtual File System) - `kernel/src/fs/fs.c`

- [ ] **VFS-1: `vfs_add_child()` race** (lines 47-54)
  - Issue: Multiple threads can corrupt `children` array - racing writes + `realloc()` can cause use-after-free
  - Fix: Acquire `vfs_lock` or add per-node lock

- [ ] **VFS-2: `node_get_child()` use-after-free** (lines 100-129)
  - Issue: Returns pointer into array that can be `realloc()`'d by concurrent `vfs_add_child()`
  - Fix: Protect with lock, or copy data instead of returning pointer

- [ ] **VFS-3: `path2node()` unprotected** (lines 132-256)
  - Issue: Called without `vfs_lock` from most callers (`fs_get_node`, `fs_mount`, `fs_symlink`, etc.)
  - Fix: Ensure all callers hold `vfs_lock`, or acquire it internally

- [ ] **VFS-4: `num_filesystems` unprotected** (line 17)
  - Issue: Global counter modified without synchronization
  - Fix: Make `_Atomic` or protect with lock

### VMM/Pagemap - `kernel/src/mem/vmm.c`, `pagemap.c`

- [ ] **VMM-1: `get_next_level()` double allocation** (vmm.c lines 24-48)
  - Issue: Two CPUs can race to allocate page table entries for same index, leaking memory
  - Fix: Use atomic compare-exchange or hold pagemap lock

- [ ] **VMM-2: Inconsistent pagemap locking**
  - Issue: `mmap_map_range()` acquires `pagemap->lock`, but `map_page()`/`unmap_page()` don't
  - Fix: Either all operations acquire lock, or document which callers must hold it

### Scheduler - `kernel/src/scheduler/scheduler.c`

- [x] **SCHED-1: `is_in_queue` non-atomic** (proc.h line 62)
  - Issue: `bool is_in_queue` accessed without synchronization from multiple CPUs
  - Fix: Changed to `_Atomic bool` and use atomic load/store

---

## Medium Priority

### TMPFS - `kernel/src/fs/tmpfs.c`

- [ ] **TMPFS-1: Non-atomic refcount initialization** (line 61)
  - Issue: `new_resource->resource.refcount = 1` uses direct write, not atomic store
  - Fix: Use `atomic_store()` for consistency

### Slab Allocator - `kernel/src/mem/slaballoc.c`

- [ ] **SLAB-1: `init_slab()` double initialization** (line 25)
  - Issue: Two threads on exhausted slab could both call `init_slab()`, wasting a page
  - Fix: Check `first_free` again after acquiring lock, or use atomic CAS

---

## Low Priority / Documentation

### PMM - `kernel/src/mem/pmm.c`

- [ ] **PMM-1: Document `pmm_lock` coverage**
  - Issue: `last_used_index` and bitmap ops are protected by `pmm_lock` but not documented
  - Fix: Add comments/annotations clarifying lock coverage

---

## Completed Fixes

- [x] **SCHED-1: `is_in_queue` non-atomic** (Fixed: commit 46adaef)
  - Issue: `bool is_in_queue` accessed without synchronization from multiple CPUs
  - Fix: Changed to `_Atomic bool` and updated all access sites to use atomic_load/atomic_store

- [x] **PROC-1: Thread creation race** (Fixed: commit 75a6a5a)
  - Issue: `process->thread_count`, `threads[]`, `thread_stack_top` accessed without lock
  - Fix: Added `threads_lock` to `process_t`, acquire in `new_user_thread()` and execve path

- [x] **SCHED-2: TOCTOU in thread selection** (Fixed: commit 8f02fe0)
  - Issue: `get_next_thread()` returned index that could become stale
  - Fix: Return thread pointer directly instead of index

---

## Notes

- VFS issues are interconnected - fixing VFS-1 through VFS-3 should be done together
- VMM-1 and VMM-2 are related - need consistent locking strategy for pagemaps
- SCHED-1 may require careful analysis of all `is_in_queue` access patterns
