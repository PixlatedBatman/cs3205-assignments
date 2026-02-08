import pyshark
from collections import defaultdict

trace_file = "Online-quiz-classroom.pcap"
pcap_info = pyshark.FileCapture(trace_file)

get_count = defaultdict(int)
streams_used = defaultdict(set)

for packet in pcap_info:
    if "HTTP" in packet and hasattr(packet.http, "request_method"):
        if packet.http.request_method == "GET":
            client_ip = packet.ip.src
            get_count[client_ip] += 1
            streams_used[client_ip].add(packet.tcp.stream)

pcap_info.close()

for client in get_count:
    print(f"Client: {client}")
    print(f"  GET requests    : {get_count[client]}")
    print(f"  TCP streams used: {len(streams_used[client])}")

    if len(streams_used[client]) == get_count[client] and get_count[client] == 1:
        print("  HTTP type       : Persistent or Non-Persistent (Cannot say)\n")
    elif len(streams_used[client]) == get_count[client]:
        print("  HTTP type       : Non-persistent\n")
    else:
        print("  HTTP type       : Persistent\n")
