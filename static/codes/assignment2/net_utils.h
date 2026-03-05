#ifndef NET_UTILS_H
#define NET_UTILS_H

#include <stdint.h>

int send_all(int sockfd, const void *buf, int len);
int recv_all(int sockfd, void *buf, int len);
int send_message(int sockfd, uint32_t type, const char *payload, uint32_t len);
int recv_message(int sockfd, uint32_t *type, char *payload, uint32_t payload_cap, uint32_t *out_len);

#endif
