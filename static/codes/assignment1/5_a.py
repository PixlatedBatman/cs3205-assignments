import pyshark
from collections import defaultdict
import matplotlib.pyplot as plt

pcap_info = pyshark.FileCapture("youtube.pcap")

throughput = defaultdict(int)

for packet in pcap_info: 
    if "TCP" in packet:
        t = int(float(packet.sniff_timestamp))
        throughput[t] += int(packet.length)

pcap_info.close()

times = sorted(throughput.keys())

# Convert to Mbps
values = [throughput[t] * 8 / 1e6 for t in times]

print("Time (s)\tThroughput (Mbps)")
start_time = times[0]

for t, v in zip(times, values):
    print(f"{t - start_time}\t\t{v:.3f}")

plt.plot([t - start_time for t in times], values)
plt.xlabel("Time (s)")
plt.ylabel("Throughput (Mbps)")
plt.title("Throughput vs Time")
plt.grid(True)
#plt.show()
plt.savefig("5a_plot_throughput.png", transparent=True, bbox_inches="tight", dpi=150)
