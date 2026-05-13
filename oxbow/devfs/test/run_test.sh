#!/bin/bash
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No color.

RESULT_DIR="results"

# Add test here.
UNIT_TEST="
	dir_test \
	file_basic \
"

# Run unit tests
run_unit() {
	for t in $UNIT_TEST; do
		echo -n "$t:"
		./"$t" &>$RESULT_DIR/"$t" && echo -e "${GREEN}PASS${NC}" || echo -e "${RED}FAIL${NC}"
	done
}

# Main
mkdir -p $RESULT_DIR

echo "Oxbow File System Unit Test Suite"
echo "--------------------------------"
run_unit | column -t -s ':'
echo "--------------------------------"
echo "Test done. Outputs are logged in './$RESULT_DIR'."
