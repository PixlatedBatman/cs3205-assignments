import pyshark

trace_file = "Online-quiz-classroom.pcap"
pcap_info = pyshark.FileCapture(trace_file)

clients = set()
server_counter = {}

for packet in pcap_info:
    if "HTTP" in packet and hasattr(packet.http, "request_method"):
        src = packet.ip.src
        dst = packet.ip.dst
        method = packet.http.request_method

        if method == "GET":
            clients.add(src)

            if dst in server_counter:
                server_counter[dst] += 1
            else:
                server_counter[dst] = 1

server = max(server_counter, key=server_counter.get)

print("Server IP: ", server)
print("Client IPs:")
for c in clients:
    print(c)

pcap_info.close()
