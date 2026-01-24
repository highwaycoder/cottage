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

## Project Overview

Cottage is an experimental x86_64 operating system with a kernel-space web server. The goal is an OS whose userspace is created primarily using JavaScript running in a browser. Written in C (GNU11) and x86_64 assembly.

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
