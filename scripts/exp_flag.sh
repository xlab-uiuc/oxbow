#!/bin/bash

# wait_flag.sh - Shell script to wait for a file flag to have a specific value
# Usage: ./wait_flag.sh <file_path> <expected_value> [timeout_seconds]

# Function to display usage
usage() {
    echo "exp_flag.sh - Shell script for file-based inter-process communication"
    echo ""
    echo "USAGE:"
    echo "  $0 <file_path> <expected_value> [timeout_seconds]  # Wait for flag value"
    echo "  $0 create <file_path> <value>                      # Create flag file with value"
    echo "  $0 read <file_path>                                # Read flag file value"
    echo "  $0 check <file_path> <expected_value>              # Check if flag has expected value"
    echo "  $0 -h|--help                                       # Show this help message"
    echo ""
    echo "COMMANDS:"
    echo "  (default)  Wait for flag file to contain expected value"
    echo "  create     Create a new flag file with specified value"
    echo "  read       Read and display the current value from flag file"
    echo "  check      Check if flag file contains expected value (exit code based)"
    echo ""
    echo "ARGUMENTS:"
    echo "  file_path       Path to the flag file to monitor/create/read"
    echo "  expected_value  Value to wait for or check against"
    echo "  value           Value to write when creating a flag file"
    echo "  timeout_seconds Optional timeout in seconds for wait operation (default: no timeout)"
    echo ""
    echo "EXAMPLES:"
    echo "  # Create a flag file with initial value"
    echo "  $0 create /tmp/sync_flag 0"
    echo ""
    echo "  # Wait for flag to become '1' (indefinitely)"
    echo "  $0 /tmp/sync_flag 1"
    echo ""
    echo "  # Wait for flag to become 'ready' with 30 second timeout"
    echo "  $0 /tmp/sync_flag ready 30"
    echo ""
    echo "  # Read current value of flag file"
    echo "  $0 read /tmp/sync_flag"
    echo ""
    echo "  # Check if flag contains specific value"
    echo "  $0 check /tmp/sync_flag 1"
    echo ""
    echo "  # Update flag file to new value"
    echo "  $0 create /tmp/sync_flag 1"
    echo ""
    echo "EXIT CODES:"
    echo "  0 - Success (expected value found, file created/read successfully)"
    echo "  1 - Error (invalid arguments, file read/write error, value mismatch)"
    echo "  2 - Timeout (wait operation timed out)"
    echo ""
    echo "NOTES:"
    echo "  - Flag files are simple text files containing a single value"
    echo "  - Useful for synchronization between different processes"
    echo "  - Wait operation checks every 0.5 seconds to minimize CPU usage"
    echo "  - Trailing newlines are automatically stripped when reading values"
    exit 1
}

# Function to create and write a flag file
create_flag() {
    local file_path="$1"
    local value="$2"
    
    if [ -z "$file_path" ] || [ -z "$value" ]; then
        echo "Error: Missing arguments for create_flag" >&2
        return 1
    fi
    
    echo "$value" > "$file_path"
    return $?
}

# Function to read flag value
read_flag() {
    local file_path="$1"
    
    if [ ! -f "$file_path" ]; then
        return 1
    fi
    
    cat "$file_path" 2>/dev/null | tr -d '\n'
}

# Function to check if flag has expected value
check_flag() {
    local file_path="$1"
    local expected_value="$2"
    
    if [ -z "$file_path" ] || [ -z "$expected_value" ]; then
        return 1
    fi
    
    local current_value
    current_value=$(read_flag "$file_path")
    
    [ "$current_value" = "$expected_value" ]
}

# Function to wait for flag value
wait_for_flag() {
    local file_path="$1"
    local expected_value="$2"
    local timeout="$3"
    local start_time
    local current_time
    local elapsed
    
    if [ -z "$file_path" ] || [ -z "$expected_value" ]; then
        echo "Error: Missing required arguments" >&2
        return 1
    fi
    
    start_time=$(date +%s)
    
    while true; do
        # Check if file has expected value
        if check_flag "$file_path" "$expected_value"; then
            echo "Flag file '$file_path' now contains expected value: '$expected_value'"
            return 0
        fi
        
        # Check timeout if specified
        if [ -n "$timeout" ] && [ "$timeout" -gt 0 ]; then
            current_time=$(date +%s)
            elapsed=$((current_time - start_time))
            
            if [ "$elapsed" -ge "$timeout" ]; then
                echo "Timeout: Flag file '$file_path' did not reach expected value '$expected_value' within $timeout seconds" >&2
                return 2
            fi
        fi
        
        # Small delay to avoid busy waiting
        sleep 0.5
    done
}

# Main script logic
main() {
    local file_path="$1"
    local expected_value="$2"
    local timeout="$3"
    
    # Check arguments
    if [ $# -lt 2 ]; then
        echo "Error: Insufficient arguments" >&2
        usage
    fi
    
    if [ $# -gt 3 ]; then
        echo "Error: Too many arguments" >&2
        usage
    fi
    
    # Validate timeout if provided
    if [ -n "$timeout" ]; then
        if ! [[ "$timeout" =~ ^[0-9]+$ ]]; then
            echo "Error: Timeout must be a positive integer" >&2
            exit 1
        fi
    fi
    
    echo "Waiting for file '$file_path' to contain value '$expected_value'..."
    if [ -n "$timeout" ]; then
        echo "Timeout: $timeout seconds"
    else
        echo "No timeout (will wait indefinitely)"
    fi
    
    wait_for_flag "$file_path" "$expected_value" "$timeout"
    exit $?
}

# Handle command line options
case "$1" in
    -h|--help)
        usage
        ;;
    create)
        # Special command to create a flag file
        # Usage: ./wait_flag.sh create <file_path> <value>
        if [ $# -ne 3 ]; then
            echo "Error: create command requires exactly 2 arguments" >&2
            echo "Usage: $0 create <file_path> <value>" >&2
            exit 1
        fi
        create_flag "$2" "$3"
        if [ $? -eq 0 ]; then
            echo "Created flag file '$2' with value '$3'"
        else
            echo "Error: Failed to create flag file '$2'" >&2
            exit 1
        fi
        ;;
    read)
        # Special command to read a flag file
        # Usage: ./wait_flag.sh read <file_path>
        if [ $# -ne 2 ]; then
            echo "Error: read command requires exactly 1 argument" >&2
            echo "Usage: $0 read <file_path>" >&2
            exit 1
        fi
        value=$(read_flag "$2")
        if [ $? -eq 0 ]; then
            echo "$value"
        else
            echo "Error: Failed to read flag file '$2'" >&2
            exit 1
        fi
        ;;
    check)
        # Special command to check a flag file
        # Usage: ./wait_flag.sh check <file_path> <expected_value>
        if [ $# -ne 3 ]; then
            echo "Error: check command requires exactly 2 arguments" >&2
            echo "Usage: $0 check <file_path> <expected_value>" >&2
            exit 1
        fi
        if check_flag "$2" "$3"; then
            echo "Flag file '$2' contains expected value '$3'"
            exit 0
        else
            echo "Flag file '$2' does not contain expected value '$3'"
            exit 1
        fi
        ;;
    *)
        # Default behavior: wait for flag
        main "$@"
        ;;
esac
