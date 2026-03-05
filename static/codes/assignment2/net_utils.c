#include "net_utils.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int send_all(int sockfd, const void *buf, int len) {
    const char *p = (const char *)buf;
    int sent = 0;
    while (sent < len) {
        int n = send(sockfd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += n;
    }
    return 0;
}

int recv_all(int sockfd, void *buf, int len) {
    char *p = (char *)buf;
    int recvd = 0;
    while (recvd < len) {
        int n = recv(sockfd, p + recvd, len - recvd, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return 1;
        }
        recvd += n;
    }
    return 0;
}

int send_message(int sockfd, uint32_t type, const char *payload, uint32_t len) {
    message_header_t h;
    h.type_net = htonl(type);
    h.length_net = htonl(len);

    if (send_all(sockfd, &h, sizeof(h)) < 0) {
        return -1;
    }
    if (len > 0 && payload != NULL) {
        if (send_all(sockfd, payload, (int)len) < 0) {
            return -1;
        }
    }
    return 0;
}

int recv_message(int sockfd, uint32_t *type, char *payload, uint32_t payload_cap, uint32_t *out_len) {
    message_header_t h;
    int rc = recv_all(sockfd, &h, sizeof(h));
    if (rc != 0) {
        return rc;
    }

    uint32_t t = ntohl(h.type_net);
    uint32_t len = ntohl(h.length_net);
    if (len > MAX_PAYLOAD || len >= payload_cap) {
        return -1;
    }

    if (len > 0) {
        rc = recv_all(sockfd, payload, (int)len);
        if (rc != 0) {
            return rc;
        }
    }
    payload[len] = '\0';

    *type = t;
    *out_len = len;
    return 0;
}
