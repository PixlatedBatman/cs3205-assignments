import pyshark
from collections import defaultdict

trace_file = "Online-quiz-classroom.pcap"
pcap_info = pyshark.FileCapture(trace_file)

connections = defaultdict(lambda: defaultdict(lambda: [None, None]))

for packet in pcap_info:
    if "TCP" not in packet:
        continue

    stream = packet.tcp.stream
    t = float(packet.sniff_timestamp)

    # SYN
    if int(packet.tcp.flags_syn) == 1 and int(packet.tcp.flags_ack) == 0:
        client_ip = packet.ip.src
        connections[client_ip][stream][0] = t

    # FIN
    if int(packet.tcp.flags_fin) == 1 or int(packet.tcp.flags_reset) == 1:
        for client in connections:
            if stream in connections[client]:
                connections[client][stream][1] = t

pcap_info.close()

def max_parallel(intervals):
    events = []
    for start, end in intervals:
        if start is not None and end is not None:
            events.append((start, +1))
            events.append((end, -1))

    events.sort()
    active = 0
    max_active = 0

    for _, delta in events:
        active += delta
        max_active = max(max_active, active)

    return max_active


print("Number of Parallel Connections:")
for client, streams in connections.items():
    intervals = list(streams.values())
    if intervals and max_parallel(intervals):
        print(f"Client {client}: ", f"{max_parallel(intervals)}")
