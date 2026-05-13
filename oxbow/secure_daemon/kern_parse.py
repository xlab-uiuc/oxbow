import re
import numpy as np

# Function to read log entries and calculate latencies
def calculate_latencies_from_file(file_path):
    async_requests = {}
    latencies = []

    with open(file_path, 'r') as file:
        for line in file:
            match = re.search(r"\[\s*([\d.]+)\] (\d+)'s nr (\d+) (async request|done)", line)
            if match:
                timestamp, pid, nr, status = match.groups()
                timestamp = float(timestamp)
                key = (pid, nr)
                if status == 'async request':
                    async_requests[key] = timestamp
                elif status == 'done' and key in async_requests:
                    start_time = async_requests.pop(key)
                    latency = (timestamp - start_time) * 1000000  # Convert to microseconds
                    if latency > 100000: 
                        print(timestamp)
                    else:
                        latencies.append(latency)

    if latencies:
        latencies_np = np.array(latencies)
        avg_latency = np.mean(latencies_np)
        min_latency = np.min(latencies_np)
        max_latency = np.max(latencies_np)
        percentile_99_latency = np.percentile(latencies_np, 99)
        percentile_999_latency = np.percentile(latencies_np, 99.9)  # Adding 99.9th percentile
        percentile_9999_latency = np.percentile(latencies_np, 99.99)
        percentile_99999_latency = np.percentile(latencies_np, 99.999)
    else:
        avg_latency = min_latency = max_latency = percentile_99_latency = percentile_999_latency = percentile_9999_latency = percentile_99999_latency = None

    return avg_latency, min_latency, max_latency, percentile_99_latency, percentile_999_latency, percentile_9999_latency, percentile_99999_latency

# Path to your log file
file_path = 'kern.log'

# Calculate latencies
results = calculate_latencies_from_file(file_path)

if results[0] is not None:
    print(f"Average Latency: {results[0]:.2f} us")
    print(f"Min Latency: {results[1]:.2f} us")
    print(f"Max Latency: {results[2]:.2f} us")
    print(f"99th Percentile Latency: {results[3]:.2f} us")
    print(f"99.9th Percentile Latency: {results[4]:.2f} us")  # Adjusted print statement
    print(f"99.99th Percentile Latency: {results[5]:.2f} us")
    print(f"99.999th Percentile Latency: {results[6]:.2f} us\n")
else:
    print("No matching async request and done pairs were found.")