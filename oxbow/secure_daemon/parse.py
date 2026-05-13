import re
import numpy as np

# Function to read microseconds values from a file and filter out values not in 0 ~ 9999 range
def read_us_values_from_file(file_path):
    us_values = []
    with open(file_path, 'r') as file:
        for line in file:
            match = re.search(r'(\d+) us', line)
            if match:
                value = int(match.group(1))
                # Only append values within the 0 ~ 9999 range
                if 0 <= value <= 9999:
                    us_values.append(value)
    return us_values

# Path to your log file
file_path = 'daemon.log'

# Read microseconds values from the file
us_values = read_us_values_from_file(file_path)

# Convert to numpy array for convenience
us_values_np = np.array(us_values)

# Calculations
avg_us = np.mean(us_values_np)
min_us = np.min(us_values_np)
max_us = np.max(us_values_np)
percentile_99_us = np.percentile(us_values_np, 99)
percentile_999_us = np.percentile(us_values_np, 99.9)
percentile_9999_us = np.percentile(us_values_np, 99.99)
percentile_99999_us = np.percentile(us_values_np, 99.999)

# Print results with two decimal places
print(f"Average: {avg_us:.2f} us")
print(f"Min: {min_us:.2f} us")
print(f"Max: {max_us:.2f} us")
print(f"99th Percentile: {percentile_99_us:.2f} us")
print(f"99.9th Percentile: {percentile_999_us:.2f} us")
print(f"99.99th Percentile: {percentile_9999_us:.2f} us")
print(f"99.999th Percentile: {percentile_99999_us:.2f} us\n")