import pyshark
from collections import defaultdict

trace_file = "Online-quiz-classroom.pcap"
pcap_info = pyshark.FileCapture(trace_file)

streams = defaultdict(set)
start_time = defaultdict(list)

for packet in pcap_info:
    if "HTTP" in packet and hasattr(packet.http, "request_uri"):
        if "raw.mp4" in packet.http.request_uri:
            client = packet.ip.src
            streams[client].add(packet.tcp.stream)
            start_time[client].append(float(packet.sniff_timestamp))

end_time = defaultdict(list)

pcap_info.close()
pcap_info = pyshark.FileCapture(trace_file)

for packet in pcap_info:
    if "TCP" in packet:
        for client in streams:
            if packet.tcp.stream in streams[client] and int(packet.tcp.len) > 0:
                end_time[client].append(float(packet.sniff_timestamp))

pcap_info.close()

print("raw.mp4 download times per client:")
for client in streams:
    t = max(end_time[client]) - min(start_time[client])
    print(f"Client {client}: {t:.2f}s")
