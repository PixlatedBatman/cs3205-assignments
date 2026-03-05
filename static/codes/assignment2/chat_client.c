#include "net_utils.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int sockfd;
    volatile int running;
} recv_ctx_t;

static int connect_tcp(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int register_with_discovery(const char *disc_ip, int disc_port, const char *username, const char *password, int client_port) {
    int fd = connect_tcp(disc_ip, disc_port);
    if (fd < 0) {
        return -1;
    }
    char payload[256];
    snprintf(payload, sizeof(payload), "%s %s %d", username, password, client_port);
    if (send_message(fd, MSG_DISC_REGISTER_REQ, payload, (uint32_t)strlen(payload)) < 0) {
        close(fd);
        return -1;
    }

    uint32_t type = 0, len = 0;
    char resp[MAX_PAYLOAD + 1];
    int rc = recv_message(fd, &type, resp, sizeof(resp), &len);
    close(fd);
    if (rc != 0) {
        return -1;
    }
    if (type == MSG_DISC_REGISTER_RESP) {
        printf("[discovery] %s\n", resp);
        return 0;
    }
    printf("[discovery error] %s\n", resp);
    return -1;
}

static void *receiver_thread(void *arg) {
    recv_ctx_t *ctx = (recv_ctx_t *)arg;
    while (ctx->running) {
        uint32_t type = 0, len = 0;
        char payload[MAX_PAYLOAD + 1];
        int rc = recv_message(ctx->sockfd, &type, payload, sizeof(payload), &len);
        if (rc != 0) {
            printf("\n[server] disconnected\n");
            ctx->running = 0;
            break;
        }

        if (type == MSG_BROADCAST_DELIVER) {
            printf("\n[broadcast] %s\n", payload);
        } else if (type == MSG_PRIVATE_DELIVER) {
            printf("\n[private] %s\n", payload);
        } else if (type == MSG_HISTORY_RESP) {
            printf("\n[history]\n%s\n", payload[0] ? payload : "(no history)");
        } else if (type == MSG_LIST_RESP) {
            printf("\n[online users]\n%s\n", payload[0] ? payload : "(none)");
        } else if (type == MSG_ACK) {
            printf("\n[ack] %s\n", payload);
        } else if (type == MSG_ERROR) {
            printf("\n[error] %s\n", payload);
        } else if (type == MSG_STATUS) {
            printf("\n[status] %s\n", payload);
        } else if (type == MSG_LOGIN_RESP) {
            printf("\n[login] %s\n", payload);
        } else {
            printf("\n[type %u] %s\n", type, payload);
        }
        fflush(stdout);
        printf("> ");
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr, "Usage: %s <disc_ip> <disc_port> <chat_ip> <chat_port> <username> <password> <client_port>\n", argv[0]);
        return 1;
    }

    const char *disc_ip = argv[1];
    int disc_port = atoi(argv[2]);
    const char *chat_ip = argv[3];
    int chat_port = atoi(argv[4]);
    const char *username = argv[5];
    const char *password = argv[6];
    int client_port = atoi(argv[7]);

    if (register_with_discovery(disc_ip, disc_port, username, password, client_port) != 0) {
        fprintf(stderr, "Discovery registration failed\n");
    }

    int chat_fd = connect_tcp(chat_ip, chat_port);
    if (chat_fd < 0) {
        fprintf(stderr, "Failed to connect to chat server\n");
        return 1;
    }

    char login_payload[128];
    snprintf(login_payload, sizeof(login_payload), "%s %s", username, password);
    if (send_message(chat_fd, MSG_LOGIN_REQ, login_payload, (uint32_t)strlen(login_payload)) < 0) {
        fprintf(stderr, "Failed to send login\n");
        close(chat_fd);
        return 1;
    }

    uint32_t type = 0, len = 0;
    char resp[MAX_PAYLOAD + 1];
    if (recv_message(chat_fd, &type, resp, sizeof(resp), &len) != 0 || type != MSG_LOGIN_RESP) {
        fprintf(stderr, "Invalid login response\n");
        close(chat_fd);
        return 1;
    }
    if (strncmp(resp, "OK", 2) != 0) {
        fprintf(stderr, "Login failed: %s\n", resp);
        close(chat_fd);
        return 1;
    }
    printf("Login successful as %s\n", username);

    recv_ctx_t ctx;
    ctx.sockfd = chat_fd;
    ctx.running = 1;
    pthread_t tid;
    pthread_create(&tid, NULL, receiver_thread, &ctx);

    char line[MAX_TEXT + MAX_USERNAME + 32];
    printf("Commands: broadcast <msg> | private <user> <msg> | list | status <available|busy|away> | history | quit\n");
    while (ctx.running) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "broadcast ", 10) == 0) {
            const char *msg = line + 10;
            send_message(chat_fd, MSG_BROADCAST_REQ, msg, (uint32_t)strlen(msg));
        } else if (strncmp(line, "private ", 8) == 0) {
            const char *msg = line + 8;
            send_message(chat_fd, MSG_PRIVATE_REQ, msg, (uint32_t)strlen(msg));
        } else if (strcmp(line, "list") == 0) {
            send_message(chat_fd, MSG_LIST_REQ, NULL, 0);
        } else if (strncmp(line, "status ", 7) == 0) {
            const char *st = line + 7;
            send_message(chat_fd, MSG_STATUS_SET_REQ, st, (uint32_t)strlen(st));
        } else if (strcmp(line, "history") == 0) {
            send_message(chat_fd, MSG_HISTORY_REQ, NULL, 0);
        } else if (strcmp(line, "quit") == 0) {
            send_message(chat_fd, MSG_DISCONNECT, NULL, 0);
            break;
        } else if (line[0] != '\0') {
            printf("Unknown command\n");
        }
    }

    ctx.running = 0;
    shutdown(chat_fd, SHUT_RDWR);
    close(chat_fd);
    pthread_join(tid, NULL);
    return 0;
}
