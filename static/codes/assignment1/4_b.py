import pyshark

trace_file = "Online-quiz-classroom.pcap"
pcap_info = pyshark.FileCapture(trace_file)

count = {}

for packet in pcap_info:
    if "HTTP" in packet and hasattr(packet.http, "request_method"):
        src = packet.ip.src

        method = packet.http.request_method
        if method == "GET":
            if src in count:
                count[src] += 1
            else:
                count[src] = 1

for client, cnt in count.items():
    print(f"{client} made {cnt} GET requests")

pcap_info.close()
