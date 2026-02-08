import pyshark
from collections import defaultdict
import matplotlib.pyplot as plt

trace_file = "Online-quiz-classroom.pcap"
pcap_info = pyshark.FileCapture(trace_file)

streams = defaultdict(set)

start_times = defaultdict(list)

for packet in pcap_info:
    if "HTTP" in packet and hasattr(packet.http, "request_uri"):
        if "raw.mp4" in packet.http.request_uri:
            client = packet.ip.src
            streams[client].add(packet.tcp.stream)
            start_times[client].append(float(packet.sniff_timestamp))

pcap_info.close()

pcap_info = pyshark.FileCapture(trace_file)

end_times = defaultdict(list)

for packet in pcap_info:
    if "TCP" in packet and int(packet.tcp.len) > 0:
        for client in streams:
            if packet.tcp.stream in streams[client]:
                end_times[client].append(float(packet.sniff_timestamp))

pcap_info.close()

clients = []
download_times = []

for client in streams:
    if start_times[client] and end_times[client]:
        dt = max(end_times[client]) - min(start_times[client])
        clients.append(client)
        download_times.append(dt)

plt.bar(clients, download_times)
plt.xlabel("Client IP")
plt.ylabel("Download Time (seconds)")
plt.title("raw.mp4 Download Time per Client")
plt.xticks(rotation=45)
plt.tight_layout()
#plt.show()
plt.savefig("4d_plot_download_times.png", transparent=True, bbox_inches="tight", dpi=150)
