#!/bin/bash
# Run all branchfs tests

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}       BranchFS Test Suite${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Check prerequisites
echo -e "${YELLOW}Checking prerequisites...${NC}"

# Check FUSE
if ! command -v fusermount3 &> /dev/null && ! command -v fusermount &> /dev/null; then
    echo -e "${RED}Error: fusermount not found. Please install fuse3.${NC}"
    exit 1
fi
echo "  ✓ FUSE available"

# Check /dev/fuse permissions
if [[ ! -r /dev/fuse ]] || [[ ! -w /dev/fuse ]]; then
    echo -e "${RED}Error: Cannot access /dev/fuse. Check permissions.${NC}"
    exit 1
fi
echo "  ✓ /dev/fuse accessible"

# Check binary exists
if [[ ! -x "$PROJECT_ROOT/target/release/branchfs" ]]; then
    echo -e "${RED}Error: target/release/branchfs not found. Run 'cargo build --release' first.${NC}"
    exit 1
fi
echo "  ✓ branchfs binary found"

# Run tests
echo ""
echo -e "${YELLOW}Running tests...${NC}"
echo ""

TOTAL_TESTS=0
PASSED_SUITES=0
FAILED_SUITES=0
FAILED_SUITE_NAMES=()

run_test_suite() {
    local test_script="$1"
    local test_name="$(basename "$test_script" .sh)"

    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}  $test_name${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    if bash "$test_script"; then
        PASSED_SUITES=$((PASSED_SUITES + 1))
        echo -e "${GREEN}Suite $test_name: PASSED${NC}"
    else
        FAILED_SUITES=$((FAILED_SUITES + 1))
        FAILED_SUITE_NAMES+=("$test_name")
        echo -e "${RED}Suite $test_name: FAILED${NC}"
    fi

    echo ""
}

# Run each test suite
for test_file in "$SCRIPT_DIR"/test_*.sh; do
    if [[ -f "$test_file" && "$test_file" != *"test_helper.sh" ]]; then
        run_test_suite "$test_file"
    fi
done

# Run ioctl integration tests
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  test_ioctl (Rust integration)${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if (cd "$PROJECT_ROOT" && cargo test --test test_ioctl -- --ignored 2>&1); then
    PASSED_SUITES=$((PASSED_SUITES + 1))
    echo -e "${GREEN}Suite test_ioctl: PASSED${NC}"
else
    FAILED_SUITES=$((FAILED_SUITES + 1))
    FAILED_SUITE_NAMES+=("test_ioctl")
    echo -e "${RED}Suite test_ioctl: FAILED${NC}"
fi
echo ""

# Run filesystem integration tests
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  test_integration (Rust integration)${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if (cd "$PROJECT_ROOT" && cargo test --test test_integration -- --ignored 2>&1); then
    PASSED_SUITES=$((PASSED_SUITES + 1))
    echo -e "${GREEN}Suite test_integration: PASSED${NC}"
else
    FAILED_SUITES=$((FAILED_SUITES + 1))
    FAILED_SUITE_NAMES+=("test_integration")
    echo -e "${RED}Suite test_integration: FAILED${NC}"
fi
echo ""

# Run libbranch C tests
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  test_branch (libbranch C integration)${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
LIBBRANCH_DIR="$PROJECT_ROOT/libbranch"
if [[ ! -x "$LIBBRANCH_DIR/tests/test_branch" ]]; then
    echo -e "${YELLOW}Building libbranch tests...${NC}"
    make -C "$LIBBRANCH_DIR" tests 2>&1
fi
if BRANCHFS_BIN="$PROJECT_ROOT/target/release/branchfs" "$LIBBRANCH_DIR/tests/test_branch" 2>&1; then
    PASSED_SUITES=$((PASSED_SUITES + 1))
    echo -e "${GREEN}Suite test_branch: PASSED${NC}"
else
    FAILED_SUITES=$((FAILED_SUITES + 1))
    FAILED_SUITE_NAMES+=("test_branch")
    echo -e "${RED}Suite test_branch: FAILED${NC}"
fi
echo ""

# Final summary
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}       Final Summary${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo "Test suites run: $TOTAL_TESTS"
echo -e "Passed: ${GREEN}$PASSED_SUITES${NC}"
echo -e "Failed: ${RED}$FAILED_SUITES${NC}"

if [[ $FAILED_SUITES -gt 0 ]]; then
    echo ""
    echo -e "${RED}Failed suites:${NC}"
    for name in "${FAILED_SUITE_NAMES[@]}"; do
        echo -e "  ${RED}✗ $name${NC}"
    done
    echo ""
    exit 1
fi

echo ""
echo -e "${GREEN}All tests passed!${NC}"
exit 0
