"""
benchmark.py
LibraryDeskSense - HTTP vs CoAP performance benchmark.
Sends N messages via each protocol, measures payload size,
round-trip time, and bandwidth. Prints a comparison table
and saves a bar chart.
"""

import asyncio
import time
import json
import statistics
import requests
import matplotlib.pyplot as plt
from aiocoap import Context, Message, Code
from aiocoap import resource

# Configuration
HTTP_URL = "http://localhost:8080/telemetry"
COAP_URL = "coap://127.0.0.1:5683/telemetry"
NUM_MESSAGES = 100

# Section 2: HTTP Benchmark
def benchmark_http(url, num_messages):
    """Send num_messages HTTP POSTs to url. Measure payload size,
    RTT, and success count. Returns a dict of results."""
    payload_sizes = []
    rtts = []
    successes = 0
    failures = 0

    for i in range(num_messages):
        payload = {
            "dist": 45.2,
            "noise": 180,
            "light": 1200,
            "occupied": True,
            "seq": i
        }
        payload_json = json.dumps(payload)
        payload_bytes = len(payload_json.encode("utf-8"))

        start = time.perf_counter()
        try:
            response = requests.post(
                url,
                data=payload_json,
                headers={"Content-Type": "application/json"},
                timeout=5
            )
            end = time.perf_counter()

            if response.status_code == 200:
                successes += 1
                rtt_ms = (end - start) * 1000
                rtts.append(rtt_ms)
                payload_sizes.append(payload_bytes)
            else:
                failures += 1
        except Exception as e:
            failures += 1
            print(f"HTTP error on message {i}: {e}")

    return {
        "protocol": "HTTP",
        "successes": successes,
        "failures": failures,
        "payload_sizes": payload_sizes,
        "rtts": rtts,
    }
# Section 3a: Minimal CoAP Server (for benchmarking) 


class TelemetryResource(resource.Resource):
    """Simple CoAP resource that accepts POSTs and replies OK."""
    async def render_post(self, request):
        return Message(code=Code.CHANGED, payload=b"ok")

async def start_coap_server():
    """Start a local CoAP server on port 5683."""
    root = resource.Site()
    root.add_resource(['telemetry'], TelemetryResource())
    await Context.create_server_context(root, bind=("127.0.0.1", 5683))

#  Section 3: CoAP Benchmark 
async def benchmark_coap(url, num_messages):
    """Send num_messages CoAP POSTs to url. Measure payload size,
    RTT, and success count. Returns a dict of results."""
    payload_sizes = []
    rtts = []
    successes = 0
    failures = 0

    context = await Context.create_client_context()

    for i in range(num_messages):
        payload = {
            "dist": 45.2,
            "noise": 180,
            "light": 1200,
            "occupied": True,
            "seq": i
        }
        payload_json = json.dumps(payload)
        payload_bytes_data = payload_json.encode("utf-8")
        payload_bytes = len(payload_bytes_data)

        request = Message(code=Code.POST, uri=url, payload=payload_bytes_data)

        start = time.perf_counter()
        try:
            response = await asyncio.wait_for(context.request(request).response, timeout=5)
            end = time.perf_counter()

            if response.code.is_successful():
                successes += 1
                rtt_ms = (end - start) * 1000
                rtts.append(rtt_ms)
                payload_sizes.append(payload_bytes)
            else:
                failures += 1
        except Exception as e:
            failures += 1
            print(f"CoAP error on message {i}: {e}")

    await context.shutdown()

    return {
        "protocol": "CoAP",
        "successes": successes,
        "failures": failures,
        "payload_sizes": payload_sizes,
        "rtts": rtts,
    }
# Section 4: Print Comparison + Save Chart
def print_comparison(http_results, coap_results):
    """Print a side-by-side comparison table of both protocols."""
    print("\n" + "=" * 60)
    print("HTTP vs CoAP - Benchmark Comparison")
    print("=" * 60)
    print(f"{'Metric':<25} {'HTTP':>15} {'CoAP':>15}")
    print("-" * 60)

    for metric, http_val, coap_val, unit in [
        ("Successful messages", http_results['successes'], coap_results['successes'], ""),
        ("Failed messages", http_results['failures'], coap_results['failures'], ""),
        ("Avg payload (bytes)", statistics.mean(http_results['payload_sizes']), statistics.mean(coap_results['payload_sizes']), ""),
        ("Avg RTT (ms)", statistics.mean(http_results['rtts']), statistics.mean(coap_results['rtts']), ""),
        ("Median RTT (ms)", statistics.median(http_results['rtts']), statistics.median(coap_results['rtts']), ""),
        ("Min RTT (ms)", min(http_results['rtts']), min(coap_results['rtts']), ""),
        ("Max RTT (ms)", max(http_results['rtts']), max(coap_results['rtts']), ""),
        ("Stdev RTT (ms)", statistics.stdev(http_results['rtts']) if len(http_results['rtts']) > 1 else 0,
                            statistics.stdev(coap_results['rtts']) if len(coap_results['rtts']) > 1 else 0, ""),
    ]:
        print(f"{metric:<25} {http_val:>15.2f} {coap_val:>15.2f}")

    print("=" * 60)


def save_comparison_chart(http_results, coap_results):
    """Save a bar chart comparing RTT and payload size."""
    metrics = ['Avg RTT (ms)', 'Avg Payload (bytes)']
    http_values = [statistics.mean(http_results['rtts']), statistics.mean(http_results['payload_sizes'])]
    coap_values = [statistics.mean(coap_results['rtts']), statistics.mean(coap_results['payload_sizes'])]

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    axes[0].bar(['HTTP', 'CoAP'], [http_values[0], coap_values[0]], color=['#2E86AB', '#A23B72'])
    axes[0].set_title('Average Round-Trip Time')
    axes[0].set_ylabel('RTT (ms)')
    axes[0].grid(True, axis='y', alpha=0.3)

    axes[1].bar(['HTTP', 'CoAP'], [http_values[1], coap_values[1]], color=['#2E86AB', '#A23B72'])
    axes[1].set_title('Average Payload Size')
    axes[1].set_ylabel('Bytes')
    axes[1].grid(True, axis='y', alpha=0.3)

    plt.tight_layout()
    plt.savefig("benchmark_comparison.png", dpi=150)
    print("\nChart saved to benchmark_comparison.png")
# Temporary test
async def main():
    print("Starting CoAP server...")
    await start_coap_server()
    await asyncio.sleep(1)
    print(f"CoAP server running on {COAP_URL}")

    print(f"\nRunning HTTP benchmark ({NUM_MESSAGES} messages)...")
    http_results = benchmark_http(HTTP_URL, NUM_MESSAGES)

    print(f"\nRunning CoAP benchmark ({NUM_MESSAGES} messages)...")
    coap_results = await benchmark_coap(COAP_URL, NUM_MESSAGES)

    print_comparison(http_results, coap_results)
    save_comparison_chart(http_results, coap_results)
    
if __name__ == "__main__":
    asyncio.run(main())