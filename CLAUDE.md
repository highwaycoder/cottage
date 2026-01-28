# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Learning Project - Explain Everything

This is a **learning/educational project**. The primary goal is understanding OS internals, not shipping quickly.

**Every code change must be explained in detail:**
- What the code does and why it's needed
- How it interacts with hardware or other kernel subsystems
- Why this approach was chosen over alternatives
- Any relevant x86_64, OS theory, or low-level concepts involved

Slower progress with thorough explanations is preferred over fast, unexplained changes.

## User Implements, Claude Guides

**Do not write code directly.** Instead:
- Discuss the problem and potential approaches
- Explain relevant concepts, tradeoffs, and gotchas
- Let the user implement the fix themselves
- Review the user's implementation and provide feedback

This maximizes learning value. Writing code for the user bypasses the educational benefit of working through the implementation details. The user learns more by:
1. Understanding the problem through discussion
2. Reasoning about the solution approach
3. Writing the code themselves
4. Getting feedback on their implementation

Exception: Trivial changes (typos, obvious one-liners) can be written directly if the user requests it.

## Project Overview

Cottage is an experimental x86_64 operating system with a kernel-space web server. The goal is an OS whose userspace is created primarily using JavaScript running in a browser. Written in C (GNU23) and x86_64 assembly.

## Build Commands

```bash
make all          # Build ISO image (cottage.iso)
make all-hdd      # Build HDD image (cottage.hdd)
make run-uefi     # Run in QEMU with UEFI (recommended)
make run          # Run in QEMU with BIOS
make test         # Alias for run-uefi
make test-headless           # Run headless (no GUI), for CI/automated testing
make test-headless TIMEOUT=30  # Run headless with 30 second timeout
make clean        # Clean build artifacts
make distclean    # Full clean including dependencies (limine, ovmf)
make ovmf         # Download nightly OVMF firmware
```

### Testing with Timeout

For automated testing or debugging, use `make test-headless TIMEOUT=<seconds>`:
- Output goes to stdout via serial console
- QEMU exits after the timeout (exit code 124 from `timeout` command, but make target succeeds)
- Without TIMEOUT, runs until crash or Ctrl+C

Debug build: `COTTAGE_DEBUG=1 make all`

### Parallel Testing for Race Conditions

**When to use parallel testing:**
- Debugging intermittent failures ("works sometimes", "fails randomly")
- Working on scheduler, SMP, or locking code
- After fixing a race condition, to verify the fix is effective
- Establishing a baseline failure rate before/after changes

**How to run:**
```bash
make test-parallel                                      # 20 runs, auto-detect parallelism
make test-parallel RUNS=50                              # More runs for better statistics
make test-parallel RUNS=30 CPUS=1                       # Single-core VMs (no SMP races)
./scripts/test-parallel.sh -v                           # Verbose mode (show failures)
./scripts/test-parallel.sh -p "panic"                   # Custom pattern (look for panics)
./scripts/test-parallel.sh -s /tmp/cottage-failures     # Save failure logs to /tmp/cottage-failures
```

**Interpreting results:**
- **95-100%**: System is stable for this workload
- **80-95%**: Intermittent race condition present
- **<80%**: Significant stability issue or test environment problem
- Compare single-core (`CPUS=1`) vs dual-core (`CPUS=2`) to isolate SMP-specific races

**Important:** The script auto-detects parallelism to avoid overcommitting host CPUs. Running too many VMs causes host contention which produces misleading failure rates.

The Makefile auto-detects CPU cores for parallel compilation. Override with `make JOBS=4` if needed.

## Architecture

### Kernel Components (`kernel/src/`)

The kernel is organized into subsystems:
- **Memory**: `pmm/` (physical), `vmm/` (virtual), `malloc/`, `slab/`
- **CPU/Interrupts**: `gdt/`, `idt/`, `isr/`, `apic/`, `smp/`
- **Scheduling**: `sched/` - basic scheduler, ~512 max threads, 2MB stack per thread
- **Filesystem**: `vfs/`, `tmpfs/`, `devtmpfs/` (ext2 WIP)
- **Drivers**: `e1000/` (network), `flanterm/` (terminal)
- **Syscalls**: `syscall/` (infrastructure present, implementation WIP)
- **ACPI**: `acpi/`, `lai/` (LAI interpreter submodule)

### Entry Points

- `kernel/src/main.c`: Kernel entry (`_start`) - initializes framebuffer, DTB, PMM, GDT, IDT, VMM, ACPI, launches scheduler
- `init/src/main.c`: Init process stub (early stage)

### Memory Layout

- Higher-half kernel mapped at `0xffffffff80000000`
- Paging mandatory (enforced by Limine bootloader)
- KASLR supported but disabled by default

### Build Structure

- `kernel/`: Kernel source and build
- `init/`: Init process/initramfs
- `limine.cfg`: Bootloader configuration
- `kernel/linker.ld`: Kernel linker script

## Code Style

- 4-space indentation, LF line endings, 120 char max line length
- Next-line brace style, spaces around operators
- Compiled with `-Werror` (warnings as errors)

## Memory Management Conventions

1. **Return values**: Any pointer returned from a function must point to heap-allocated memory or NULL (never stack/static). Must be safe to `free()`.

2. **String pointers in structs**: Always copy strings into structures with their own allocated memory. Free when structure lifetime ends.

## Branching Strategy

- `master`: Production branch for nightly builds, must stay stable
- `develop`: Integration branch for PRs
- Feature branches target `develop`, never `master` directly
- PRs require an associated GitHub issue first

## Dependencies

- Limine v5.x bootloader (cloned during build)
- LAI (ACPI interpreter) as git submodule at `kernel/src/lai/`
- OVMF firmware for UEFI testing (optional, downloaded via `make ovmf`)
- Build tools: clang/gcc, nasm, xorriso, git, curl
