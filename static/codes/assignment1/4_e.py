import pyshark

trace_file = "Online-quiz-classroom.pcap"
pcap_info = pyshark.FileCapture(trace_file)

client_ip = None
for packet in pcap_info:
    if "HTTP" in packet and hasattr(packet.http, "request_method"):
        if packet.http.request_method == "GET":
            client_ip = packet.ip.src
            break
print("Client IP used:", client_ip, "\n")

user_agent = None
status_code = None
status_phrase = None
server_header = None

for packet in pcap_info:
    if user_agent and status_code and status_phrase and server_header:
        break

    if "HTTP" in packet:
        # HTTP request
        if packet.ip.src == client_ip and hasattr(packet.http, "user_agent") and user_agent is None:
            user_agent = packet.http.user_agent

        # HTTP response
        if hasattr(packet.http, "response_code") and status_code is None:
            status_code = packet.http.response_code
            status_phrase = packet.http.response_phrase

        if hasattr(packet.http, "server"):
            server_header = packet.http.server

print("User-Agent:", user_agent)
print("HTTP Response Code:", status_code)
print("Response Description:", status_phrase)
print("Web Server:", server_header)

pcap_info.close()
