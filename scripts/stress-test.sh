#!/bin/bash
#
# Scheduler Race Condition Stress Test Harness
# =============================================
#
# This script runs the kernel repeatedly with stress tests enabled,
# looking for crashes or invariant violations that indicate race conditions.
#
# The key insight is that race conditions are probabilistic - a race might
# only trigger 1 in 1000 runs. By running many times and collecting results,
# we can catch races that would be nearly impossible to find manually.
#
# Usage:
#   ./scripts/stress-test.sh              # Run 10 iterations with default timeout
#   ./scripts/stress-test.sh 50           # Run 50 iterations
#   ./scripts/stress-test.sh 50 60        # Run 50 iterations, 60 second timeout each
#
# Exit codes:
#   0 - All runs completed without detecting races
#   1 - At least one run detected a race or crashed
#

set -e

ITERATIONS=${1:-10}
TIMEOUT=${2:-30}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="$PROJECT_DIR/stress-results"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================"
echo "  Scheduler Stress Test Harness"
echo "========================================"
echo ""
echo "Configuration:"
echo "  Iterations: $ITERATIONS"
echo "  Timeout per run: ${TIMEOUT}s"
echo "  Results directory: $RESULTS_DIR"
echo ""

# Create results directory
mkdir -p "$RESULTS_DIR"

# Build the kernel with stress tests enabled
echo "Building kernel..."
cd "$PROJECT_DIR"
make clean > /dev/null 2>&1 || true
COTTAGE_STRESS_TEST=1 make all > "$RESULTS_DIR/build.log" 2>&1

if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed! See $RESULTS_DIR/build.log${NC}"
    exit 1
fi

echo -e "${GREEN}Build successful${NC}"
echo ""

# Track results
TOTAL_RUNS=0
SUCCESSFUL_RUNS=0
RACE_DETECTED=0
CRASHES=0
TIMEOUTS=0

# Run the stress tests multiple times
echo "Starting stress test runs..."
echo ""

for i in $(seq 1 $ITERATIONS); do
    TOTAL_RUNS=$((TOTAL_RUNS + 1))
    LOG_FILE="$RESULTS_DIR/run-$i.log"

    printf "Run %3d/%d: " $i $ITERATIONS

    # Run QEMU with timeout, capture output
    # The stress test kernel will print results to serial
    timeout ${TIMEOUT}s make test-headless > "$LOG_FILE" 2>&1
    EXIT_CODE=$?

    # Analyze the output
    if [ $EXIT_CODE -eq 124 ]; then
        # Timeout - check if stress tests completed
        if grep -q "ALL TESTS PASSED" "$LOG_FILE"; then
            echo -e "${GREEN}PASSED${NC} (timeout after completion)"
            SUCCESSFUL_RUNS=$((SUCCESSFUL_RUNS + 1))
        elif grep -q "INVARIANT VIOLATION" "$LOG_FILE"; then
            echo -e "${RED}RACE DETECTED${NC} - see $LOG_FILE"
            RACE_DETECTED=$((RACE_DETECTED + 1))
        elif grep -q "RACE DETECTED" "$LOG_FILE"; then
            echo -e "${RED}RACE DETECTED${NC} - see $LOG_FILE"
            RACE_DETECTED=$((RACE_DETECTED + 1))
        else
            echo -e "${YELLOW}TIMEOUT${NC} (tests didn't complete)"
            TIMEOUTS=$((TIMEOUTS + 1))
        fi
    elif [ $EXIT_CODE -ne 0 ]; then
        # QEMU exited abnormally - check for panic
        if grep -q "KERNEL PANIC" "$LOG_FILE" || grep -q "panic:" "$LOG_FILE"; then
            echo -e "${RED}CRASH${NC} - see $LOG_FILE"
            CRASHES=$((CRASHES + 1))

            # Extract panic message for quick diagnosis
            echo "  -> $(grep -m1 'panic:' "$LOG_FILE" || grep -m1 'KERNEL PANIC' "$LOG_FILE")"
        else
            echo -e "${RED}ERROR (exit code $EXIT_CODE)${NC} - see $LOG_FILE"
            CRASHES=$((CRASHES + 1))
        fi
    else
        # Normal exit
        if grep -q "ALL TESTS PASSED" "$LOG_FILE"; then
            echo -e "${GREEN}PASSED${NC}"
            SUCCESSFUL_RUNS=$((SUCCESSFUL_RUNS + 1))
        elif grep -q "INVARIANT VIOLATION" "$LOG_FILE"; then
            echo -e "${RED}RACE DETECTED${NC} - see $LOG_FILE"
            RACE_DETECTED=$((RACE_DETECTED + 1))
        elif grep -q "SOME TESTS FAILED" "$LOG_FILE"; then
            echo -e "${RED}FAILED${NC} - see $LOG_FILE"
            RACE_DETECTED=$((RACE_DETECTED + 1))
        else
            echo -e "${YELLOW}UNKNOWN${NC} - see $LOG_FILE"
            TIMEOUTS=$((TIMEOUTS + 1))
        fi
    fi
done

# Print summary
echo ""
echo "========================================"
echo "  STRESS TEST SUMMARY"
echo "========================================"
echo "Total runs:      $TOTAL_RUNS"
echo -e "Successful:      ${GREEN}$SUCCESSFUL_RUNS${NC}"
echo -e "Races detected:  ${RED}$RACE_DETECTED${NC}"
echo -e "Crashes:         ${RED}$CRASHES${NC}"
echo -e "Timeouts:        ${YELLOW}$TIMEOUTS${NC}"
echo ""

# Calculate failure rate
FAILURES=$((RACE_DETECTED + CRASHES))
if [ $TOTAL_RUNS -gt 0 ]; then
    FAILURE_RATE=$(echo "scale=2; $FAILURES * 100 / $TOTAL_RUNS" | bc)
    echo "Failure rate: ${FAILURE_RATE}%"
fi

# Provide guidance on results
echo ""
if [ $FAILURES -eq 0 ]; then
    echo -e "${GREEN}No races detected in $TOTAL_RUNS runs.${NC}"
    echo ""
    echo "This doesn't prove no races exist - it means we didn't hit any."
    echo "Consider:"
    echo "  - Running more iterations (./scripts/stress-test.sh 100)"
    echo "  - Increasing stress test thread count in kernel config"
    echo "  - Running on hardware with more CPU cores"
    exit 0
else
    echo -e "${RED}Potential race conditions detected!${NC}"
    echo ""
    echo "Review the log files in $RESULTS_DIR for details."
    echo ""
    echo "Common patterns to look for:"
    echo "  - 'INVARIANT VIOLATION' - shows which invariant was broken"
    echo "  - 'RACE DETECTED' - shows what state corruption occurred"
    echo "  - 'panic:' - shows what crash happened"
    echo ""
    echo "Failed run logs:"
    for log in "$RESULTS_DIR"/*.log; do
        if grep -q "INVARIANT VIOLATION\|RACE DETECTED\|panic:" "$log" 2>/dev/null; then
            echo "  - $log"
        fi
    done
    exit 1
fi
