# Import the necessary module
import re
from collections import defaultdict

# Initialize a dictionary to store counts of each "nr" number
nr_counts = defaultdict(int)

# Open and read the daemon.log file
with open('daemon.log', 'r') as file:
    for line in file:
        # Use regular expression to find patterns like "nr 1", "nr 2", etc.
        match = re.search(r'nr (\d+)', line)
        if match:
            # Extract the number following "nr" and convert it to an integer
            number = int(match.group(1))
            # Increment the count for this number
            if 1 <= number <= 32:  # Ensure the number is within the specified range
                nr_counts[number] += 1

# Sort the dictionary by its keys (number) and then print
for nr in sorted(nr_counts):
    print(f'nr {nr} is {nr_counts[nr]}')