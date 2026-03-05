#define _POSIX_C_SOURCE 200809L

#include "net_utils.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int sockfd;
    volatile int running;
    char username[MAX_USERNAME];
    long long latency_sum_us;
    int latency_count;
    long long latency_min_us;
    long long latency_max_us;
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
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int register_user(
    const char *disc_ip,
    int disc_port,
    const char *username,
    const char *password,
    int client_port
) {
    int fd = connect_tcp(disc_ip, disc_port);
    if (fd < 0) {
        return -1;
    }

    char payload[256];
    snprintf(payload, sizeof(payload), "%s %s %d", username, password, client_port);
    if (send_message(fd, MSG_DISC_REGISTER_REQ, payload, (uint32_t)strlen(payload)) != 0) {
        close(fd);
        return -1;
    }

    uint32_t type = 0, len = 0;
    char resp[MAX_PAYLOAD + 1];
    int rc = recv_message(fd, &type, resp, sizeof(resp), &len);
    close(fd);
    if (rc != 0 || type != MSG_DISC_REGISTER_RESP) {
        return -1;
    }
    return 0;
}

static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000000LL + (long long)(ts.tv_nsec / 1000LL);
}

static void sleep_us(int delay_us) {
    if (delay_us <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = delay_us / 1000000;
    ts.tv_nsec = (long)(delay_us % 1000000) * 1000L;
    nanosleep(&ts, NULL);
}

static void *receiver_worker(void *arg) {
    recv_ctx_t *ctx = (recv_ctx_t *)arg;
    while (ctx->running) {
        uint32_t type = 0, len = 0;
        char payload[MAX_PAYLOAD + 1];
        int rc = recv_message(ctx->sockfd, &type, payload, sizeof(payload), &len);
        if (rc != 0) {
            break;
        }

        if (type == MSG_BROADCAST_DELIVER) {
            char from[MAX_USERNAME];
            char text[MAX_TEXT];
            if (sscanf(payload, "%31s %[^\n]", from, text) == 2 && strcmp(from, ctx->username) != 0) {
                int seq = 0;
                long long ts_us = 0;
                if (sscanf(text, "mode=%*s user=%*s seq=%d ts_us=%lld", &seq, &ts_us) == 2) {
                    (void)seq;
                    long long latency_us = now_us() - ts_us;
                    if (latency_us >= 0) {
                        if (ctx->latency_count == 0 || latency_us < ctx->latency_min_us) {
                            ctx->latency_min_us = latency_us;
                        }
                        if (ctx->latency_count == 0 || latency_us > ctx->latency_max_us) {
                            ctx->latency_max_us = latency_us;
                        }
                        ctx->latency_sum_us += latency_us;
                        ctx->latency_count++;
                        printf("LATENCY_US %lld\n", latency_us);
                        fflush(stdout);
                    }
                }
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 11) {
        fprintf(
            stderr,
            "Usage: %s <disc_ip> <disc_port> <chat_ip> <chat_port> <username> <password> <client_port> <messages> <prefix> <delay_us>\n",
            argv[0]
        );
        return 2;
    }

    const char *disc_ip = argv[1];
    int disc_port = atoi(argv[2]);
    const char *chat_ip = argv[3];
    int chat_port = atoi(argv[4]);
    const char *username = argv[5];
    const char *password = argv[6];
    int client_port = atoi(argv[7]);
    int messages = atoi(argv[8]);
    const char *prefix = argv[9];
    int delay_us = atoi(argv[10]);
    if (messages <= 0) {
        fprintf(stderr, "messages must be > 0\n");
        return 2;
    }

    if (register_user(disc_ip, disc_port, username, password, client_port) != 0) {
        fprintf(stderr, "register failed for %s\n", username);
        return 1;
    }

    int fd = connect_tcp(chat_ip, chat_port);
    if (fd < 0) {
        fprintf(stderr, "chat connect failed for %s\n", username);
        return 1;
    }

    char login[128];
    snprintf(login, sizeof(login), "%s %s", username, password);
    if (send_message(fd, MSG_LOGIN_REQ, login, (uint32_t)strlen(login)) != 0) {
        close(fd);
        return 1;
    }

    uint32_t type = 0, len = 0;
    char resp[MAX_PAYLOAD + 1];
    if (recv_message(fd, &type, resp, sizeof(resp), &len) != 0 || type != MSG_LOGIN_RESP || strncmp(resp, "OK", 2) != 0) {
        fprintf(stderr, "login failed for %s: %s\n", username, resp);
        close(fd);
        return 1;
    }

    recv_ctx_t ctx;
    ctx.sockfd = fd;
    ctx.running = 1;
    strncpy(ctx.username, username, sizeof(ctx.username) - 1);
    ctx.username[sizeof(ctx.username) - 1] = '\0';
    ctx.latency_sum_us = 0;
    ctx.latency_count = 0;
    ctx.latency_min_us = 0;
    ctx.latency_max_us = 0;
    pthread_t recv_tid;
    if (pthread_create(&recv_tid, NULL, receiver_worker, &ctx) != 0) {
        close(fd);
        return 1;
    }

    for (int i = 0; i < messages; i++) {
        char msg[MAX_TEXT];
        int seq = i + 1;
        long long ts_us = now_us();
        snprintf(msg, sizeof(msg), "mode=%s user=%s seq=%d ts_us=%lld", prefix, username, seq, ts_us);
        if (send_message(fd, MSG_BROADCAST_REQ, msg, (uint32_t)strlen(msg)) != 0) {
            ctx.running = 0;
            shutdown(fd, SHUT_RDWR);
            pthread_join(recv_tid, NULL);
            close(fd);
            return 1;
        }
        sleep_us(delay_us);
    }

    sleep_us(200000);
    (void)send_message(fd, MSG_DISCONNECT, NULL, 0);
    ctx.running = 0;
    shutdown(fd, SHUT_RDWR);
    pthread_join(recv_tid, NULL);
    close(fd);

    if (ctx.latency_count > 0) {
        double avg = (double)ctx.latency_sum_us / (double)ctx.latency_count;
        fprintf(stderr, "LAT_SUMMARY count=%d avg_us=%.2f min_us=%lld max_us=%lld\n",
                ctx.latency_count, avg, ctx.latency_min_us, ctx.latency_max_us);
    } else {
        fprintf(stderr, "LAT_SUMMARY count=0 avg_us=0 min_us=0 max_us=0\n");
    }

    return 0;
}
