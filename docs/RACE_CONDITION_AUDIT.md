# Race Condition Audit Checklist

This document provides guidance for identifying and preventing race conditions in the Cottage kernel. Use this checklist when reviewing code or adding new features.

## What is a Race Condition?

A race condition occurs when:
1. Two or more execution contexts (threads, CPUs, interrupt handlers) access shared data
2. At least one access is a write
3. The accesses are not properly synchronized

The result depends on the relative timing of the accesses, leading to unpredictable behavior.

## Quick Reference: Common Race Patterns

| Pattern | Example | Fix |
|---------|---------|-----|
| Check-then-act | `if (!locked) { locked = true; }` | Use atomic CAS |
| Read-modify-write | `counter++` | Use `atomic_fetch_add` |
| TOCTOU | Read index, then use it | Return actual pointer |
| Double-checked locking | Check, lock, check again | Don't do this |
| Unprotected struct fields | `obj->field = value` without lock | Add lock or make atomic |

## Audit Checklist

### 1. Identify Shared State

For each data structure, ask:
- [ ] Can this be accessed from multiple CPUs simultaneously?
- [ ] Can this be accessed from both thread context and interrupt context?
- [ ] Can this be accessed during thread creation/destruction?

**Red flags:**
- Global variables without `static`
- Struct fields accessed without holding a lock
- Pointers passed between threads

### 2. Check Synchronization

For each piece of shared state:
- [ ] Is there a lock protecting it?
- [ ] Is the lock ALWAYS held when accessing?
- [ ] Is the lock documented (comment or `GUARDED_BY` annotation)?

**Use Clang annotations:**
```c
typedef struct {
    lock_t lock;
    int counter GUARDED_BY(lock);  // Clang will warn if accessed without lock
} my_struct_t;
```

### 3. Check Atomic Operations

For simple counters/flags that don't need locks:
- [ ] Is the variable declared `_Atomic`?
- [ ] Are you using `atomic_load`/`atomic_store`/`atomic_fetch_add`?
- [ ] NOT just volatile (volatile != atomic!)

**Wrong:**
```c
volatile int counter;
counter++;  // NOT ATOMIC - this is read-modify-write
```

**Right:**
```c
_Atomic int counter;
atomic_fetch_add(&counter, 1);  // Atomic increment
```

### 4. Check Lock Ordering

If code acquires multiple locks:
- [ ] Is there a consistent order? (Always A before B)
- [ ] Is the order documented?
- [ ] Are there any paths that acquire in different order? (Deadlock!)

**Document lock ordering:**
```c
// Lock ordering: Always acquire process->lock before thread->lock
// Violating this order will cause deadlock
```

### 5. Check Interrupt Safety

For code that might run with interrupts enabled:
- [ ] Does it access per-CPU data? (GS segment)
- [ ] If yes, are interrupts disabled first?
- [ ] Is the critical section as short as possible?

**Pattern:**
```c
// Per-CPU data access requires interrupts disabled
asm volatile("cli");
local_cpu_t* cpu = cpu_get_current();
// ... use cpu ...
asm volatile("sti");
```

### 6. Check Initialization Races

For lazily-initialized data:
- [ ] Can two threads race to initialize?
- [ ] Is there a "initialized" flag?
- [ ] Is the flag checked atomically?

**Wrong:**
```c
if (!initialized) {
    do_init();  // Two threads could both enter here!
    initialized = true;
}
```

**Right:**
```c
static _Atomic bool initialized = false;
bool expected = false;
if (atomic_compare_exchange_strong(&initialized, &expected, true)) {
    do_init();  // Only one thread enters
}
```

### 7. Check Pointer Lifetime

For data accessed via pointers:
- [ ] Can the pointed-to object be freed while pointer is in use?
- [ ] Is there reference counting or other lifetime management?
- [ ] After getting a pointer from a lookup, can it become invalid?

**Red flags:**
- Storing pointers from lookups without incrementing refcount
- Using pointers after releasing locks
- Returning pointers to stack-allocated data

### 8. Check Signal/Event Handling

For code that waits for or delivers signals/events:
- [ ] Is the wait condition checked atomically with the sleep?
- [ ] Can the signal be delivered before we start waiting?
- [ ] Is there a race between checking and sleeping?

## Specific Areas in Cottage

### Scheduler (`kernel/src/scheduler/`)
- `scheduler_running_queue[]` - Protected by per-thread locks + atomic ops
- `thread->cpuid` - Atomic, indicates which CPU owns thread
- `thread->is_in_queue` - **CAUTION**: Non-atomic, potential races
- `working_cpus` - Atomic counter

### Process/Thread (`kernel/src/proc/`)
- `process->threads[]` - **NEEDS AUDIT**: No lock protection currently
- `process->thread_count` - **NEEDS AUDIT**: No lock protection currently
- `process->fds[]` - Protected by `fds_lock`

### Memory (`kernel/src/mem/`)
- PMM bitmap - Needs audit for SMP safety
- VMM page tables - Needs audit for concurrent mapping
- Slab allocator - Needs audit for concurrent alloc/free

### File System (`kernel/src/fs/`)
- VFS node tree - Needs lock per node or global lock
- File descriptors - Protected by process `fds_lock`

## Tools Available

### Compile-Time: Clang Thread Safety Analysis
Enabled by `-Wthread-safety`. Use annotations:
- `GUARDED_BY(lock)` - Field protected by lock
- `REQUIRES(lock)` - Function needs lock held
- `ACQUIRE(lock)` / `RELEASE(lock)` - Function acquires/releases
- `NO_THREAD_SAFETY_ANALYSIS` - Disable for complex cases

### Runtime: Debug Lock Instrumentation
Build with `COTTAGE_DEBUG=1`. Detects:
- Recursive locking (instant deadlock)
- Releasing lock not held
- Releasing lock held by different CPU
- Long lock waits (potential deadlock)

### Testing: Stress Tests
Run `make stress-test` to exercise scheduler with high concurrency.

## Adding New Shared State

When adding new shared data:

1. **Document the synchronization strategy** in a comment:
   ```c
   // Protected by: foo_lock
   // Accessed from: thread context only
   // Lifetime: allocated at init, never freed
   ```

2. **Add appropriate annotations**:
   ```c
   struct foo {
       lock_t lock;
       int data GUARDED_BY(lock);
   };
   ```

3. **Add to this audit document** if it's a significant new subsystem

4. **Consider adding stress tests** if the data is accessed frequently

## References

- [Clang Thread Safety Analysis](https://clang.llvm.org/docs/ThreadSafetyAnalysis.html)
- [Linux Kernel Locking Guide](https://www.kernel.org/doc/html/latest/locking/locktypes.html)
- [Memory Barriers](https://www.kernel.org/doc/html/latest/core-api/wrappers/memory-barriers.html)
