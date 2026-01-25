#!/bin/bash
#
# Parallel boot test runner for Cottage OS
#
# Runs multiple QEMU instances in parallel to gather failure rate statistics.
# Useful for debugging race conditions where failures are intermittent.
#
# Usage:
#   ./scripts/test-parallel.sh [OPTIONS]
#
# Examples:
#   ./scripts/test-parallel.sh                     # 10 runs, auto-parallel, 2 CPUs
#   ./scripts/test-parallel.sh -n 50               # 50 runs
#   ./scripts/test-parallel.sh -n 20 -c 1 -j 12    # 20 runs, 1 CPU each, 12 parallel
#   ./scripts/test-parallel.sh -p "hello"          # Custom success pattern
#   ./scripts/test-parallel.sh -v                  # Verbose (show all output)

set -e

# Default configuration
TOTAL_RUNS=10
CPUS_PER_VM=2
TIMEOUT_SECS=10
SUCCESS_PATTERN="hello, syscalls"
VERBOSE=false
PARALLEL_JOBS=""  # Auto-detect if not specified
MEMORY_MB=512
ISO_PATH="cottage.iso"
OVMF_PATH="ovmf/OVMF.fd"
SAVE_FAILURES=""  # Directory to save failure logs

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Parallel boot test runner for Cottage OS

Options:
  -n, --runs NUM        Total number of test runs (default: $TOTAL_RUNS)
  -c, --cpus NUM        CPUs per VM (default: $CPUS_PER_VM)
  -j, --jobs NUM        Parallel jobs (default: auto-detect based on CPUs)
  -t, --timeout SECS    Timeout per test in seconds (default: $TIMEOUT_SECS)
  -p, --pattern STR     Success pattern to grep for (default: "$SUCCESS_PATTERN")
  -m, --memory MB       Memory per VM in MB (default: $MEMORY_MB)
  -i, --iso PATH        Path to ISO file (default: $ISO_PATH)
  -s, --save-failures DIR  Save failure logs to directory
  -v, --verbose         Show output from each test
  -q, --quiet           Minimal output (just final stats)
  -h, --help            Show this help message

Examples:
  $0 -n 50 -c 1 -j 12   # 50 tests, single-core VMs, 12 parallel
  $0 -n 20 -c 2 -j 6    # 20 tests, dual-core VMs, 6 parallel
  $0 -p "panic"         # Look for panics instead of success
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -n|--runs)
            TOTAL_RUNS="$2"
            shift 2
            ;;
        -c|--cpus)
            CPUS_PER_VM="$2"
            shift 2
            ;;
        -j|--jobs)
            PARALLEL_JOBS="$2"
            shift 2
            ;;
        -t|--timeout)
            TIMEOUT_SECS="$2"
            shift 2
            ;;
        -p|--pattern)
            SUCCESS_PATTERN="$2"
            shift 2
            ;;
        -m|--memory)
            MEMORY_MB="$2"
            shift 2
            ;;
        -i|--iso)
            ISO_PATH="$2"
            shift 2
            ;;
        -s|--save-failures)
            SAVE_FAILURES="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -q|--quiet)
            QUIET=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Auto-detect parallel jobs if not specified
if [[ -z "$PARALLEL_JOBS" ]]; then
    HOST_CPUS=$(nproc)
    # Calculate max parallel jobs so total vCPUs don't exceed host CPUs
    PARALLEL_JOBS=$((HOST_CPUS / CPUS_PER_VM))
    # Ensure at least 1 job
    [[ $PARALLEL_JOBS -lt 1 ]] && PARALLEL_JOBS=1
fi

# Warn if we're overcommitting
TOTAL_VCPUS=$((PARALLEL_JOBS * CPUS_PER_VM))
HOST_CPUS=$(nproc)
if [[ $TOTAL_VCPUS -gt $HOST_CPUS ]]; then
    echo -e "${YELLOW}Warning: Running $PARALLEL_JOBS VMs x $CPUS_PER_VM CPUs = $TOTAL_VCPUS vCPUs on $HOST_CPUS host CPUs${NC}"
    echo -e "${YELLOW}         Results may be unreliable due to CPU contention${NC}"
    echo ""
fi

# Check prerequisites
if [[ ! -f "$ISO_PATH" ]]; then
    echo -e "${RED}Error: ISO not found at $ISO_PATH${NC}"
    echo "Run 'make all' first to build the ISO"
    exit 1
fi

if [[ ! -f "$OVMF_PATH" ]]; then
    echo -e "${RED}Error: OVMF firmware not found at $OVMF_PATH${NC}"
    echo "Run 'make ovmf' to download it"
    exit 1
fi

# Create temp directory for results
RESULTS_DIR=$(mktemp -d)
trap "rm -rf $RESULTS_DIR" EXIT

# Print configuration
if [[ "$QUIET" != "true" ]]; then
    echo -e "${BLUE}Cottage OS Parallel Test Runner${NC}"
    echo "================================"
    echo "Total runs:      $TOTAL_RUNS"
    echo "Parallel jobs:   $PARALLEL_JOBS"
    echo "CPUs per VM:     $CPUS_PER_VM"
    echo "Timeout:         ${TIMEOUT_SECS}s"
    echo "Success pattern: \"$SUCCESS_PATTERN\""
    echo "Memory per VM:   ${MEMORY_MB}MB"
    echo ""
fi

# Function to run a single test
run_single_test() {
    local test_num=$1
    local output_file="$RESULTS_DIR/test_$test_num.log"
    local result_file="$RESULTS_DIR/test_$test_num.result"

    # Run QEMU with timeout
    # Use snapshot=on to allow parallel read-only access to the ISO
    # Suppress stderr (QEMU termination messages) for cleaner output
    timeout "${TIMEOUT_SECS}s" qemu-system-x86_64 \
        -bios "$OVMF_PATH" \
        -drive file="$ISO_PATH",format=raw,snapshot=on \
        -serial stdio \
        -display none \
        -m "$MEMORY_MB" \
        -smp "$CPUS_PER_VM" \
        2>/dev/null > "$output_file" || true

    # Check for success pattern
    if grep -q "$SUCCESS_PATTERN" "$output_file" 2>/dev/null; then
        echo "success" > "$result_file"
        if [[ "$VERBOSE" == "true" ]]; then
            echo -e "${GREEN}Test $test_num: PASS${NC}"
        fi
    else
        echo "fail" > "$result_file"
        if [[ "$VERBOSE" == "true" ]]; then
            echo -e "${RED}Test $test_num: FAIL${NC}"
            # Show last few lines of output for failed tests
            echo "  Last output:"
            tail -5 "$output_file" 2>/dev/null | sed 's/^/    /'
        fi
        # Save failure log if requested
        if [[ -n "$SAVE_FAILURES" ]]; then
            cp "$output_file" "$SAVE_FAILURES/fail_$test_num.log"
        fi
    fi
}

export -f run_single_test
export RESULTS_DIR TIMEOUT_SECS OVMF_PATH ISO_PATH MEMORY_MB CPUS_PER_VM
export SUCCESS_PATTERN VERBOSE SAVE_FAILURES RED GREEN NC

# Create save directory if specified
if [[ -n "$SAVE_FAILURES" ]]; then
    mkdir -p "$SAVE_FAILURES"
    echo "Failure logs will be saved to: $SAVE_FAILURES"
fi

# Run tests in parallel
if [[ "$QUIET" != "true" ]]; then
    echo -e "${YELLOW}Running $TOTAL_RUNS tests ($PARALLEL_JOBS parallel)...${NC}"
fi

# Use seq to generate test numbers and GNU parallel or xargs for parallel execution
if command -v parallel &> /dev/null; then
    # Use GNU parallel if available (better job control)
    seq 1 "$TOTAL_RUNS" | parallel -j "$PARALLEL_JOBS" run_single_test {}
else
    # Fall back to xargs
    seq 1 "$TOTAL_RUNS" | xargs -P "$PARALLEL_JOBS" -I {} bash -c 'run_single_test "$@"' _ {}
fi

# Count results
SUCCESS_COUNT=$(grep -l "success" "$RESULTS_DIR"/*.result 2>/dev/null | wc -l)
FAIL_COUNT=$(grep -l "fail" "$RESULTS_DIR"/*.result 2>/dev/null | wc -l)
TOTAL_COUNTED=$((SUCCESS_COUNT + FAIL_COUNT))

# Calculate percentage
if [[ $TOTAL_COUNTED -gt 0 ]]; then
    SUCCESS_RATE=$((SUCCESS_COUNT * 100 / TOTAL_COUNTED))
else
    SUCCESS_RATE=0
fi

# Print summary
echo ""
echo -e "${BLUE}Results${NC}"
echo "======="
echo -e "Success: ${GREEN}$SUCCESS_COUNT${NC}/$TOTAL_COUNTED ($SUCCESS_RATE%)"
echo -e "Failed:  ${RED}$FAIL_COUNT${NC}/$TOTAL_COUNTED"

# Exit with appropriate code
if [[ $FAIL_COUNT -gt 0 ]]; then
    exit 1
else
    exit 0
fi
