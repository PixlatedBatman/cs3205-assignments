#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_USERNAME 32
#define MAX_PASSWORD 32
#define MAX_TEXT 1024
#define MAX_PAYLOAD 4096

typedef enum {
    MSG_DISC_REGISTER_REQ = 1,
    MSG_DISC_REGISTER_RESP = 2,
    MSG_DISC_LOOKUP_REQ = 3,
    MSG_DISC_LOOKUP_RESP = 4,

    MSG_LOGIN_REQ = 10,
    MSG_LOGIN_RESP = 11,
    MSG_BROADCAST_REQ = 12,
    MSG_BROADCAST_DELIVER = 13,
    MSG_PRIVATE_REQ = 14,
    MSG_PRIVATE_DELIVER = 15,
    MSG_LIST_REQ = 16,
    MSG_LIST_RESP = 17,
    MSG_STATUS = 18,
    MSG_ACK = 19,
    MSG_ERROR = 20,
    MSG_DISCONNECT = 21,
    MSG_STATUS_SET_REQ = 22,
    MSG_HISTORY_REQ = 23,
    MSG_HISTORY_RESP = 24
} message_type_t;

typedef struct {
    uint32_t type_net;
    uint32_t length_net;
} message_header_t;

#endif
