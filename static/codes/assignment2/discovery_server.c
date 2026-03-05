#include "net_utils.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BACKLOG 16
#define REGISTRY_PATH "data/registry.txt"

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

static int ensure_registry_file(void) {
    FILE *fp = fopen(REGISTRY_PATH, "a");
    if (!fp) {
        perror("fopen registry");
        return -1;
    }
    fclose(fp);
    return 0;
}

static int upsert_user(const char *username, const char *password, const char *ip, int port) {
    FILE *in = fopen(REGISTRY_PATH, "r");
    FILE *tmp = fopen("data/registry.tmp", "w");
    int found = 0;
    char u[MAX_USERNAME], p[MAX_PASSWORD], i[64];
    int po = 0;

    if (!tmp) {
        if (in) fclose(in);
        return -1;
    }

    if (in) {
        while (fscanf(in, "%31s %31s %63s %d", u, p, i, &po) == 4) {
            if (strcmp(u, username) == 0) {
                fprintf(tmp, "%s %s %s %d\n", username, password, ip, port);
                found = 1;
            } else {
                fprintf(tmp, "%s %s %s %d\n", u, p, i, po);
            }
        }
        fclose(in);
    }

    if (!found) {
        fprintf(tmp, "%s %s %s %d\n", username, password, ip, port);
    }
    fclose(tmp);
    if (rename("data/registry.tmp", REGISTRY_PATH) != 0) {
        return -1;
    }
    return 0;
}

static int lookup_user_with_password(const char *username, char *out_password, size_t out_password_sz, char *out_ip, int *out_port) {
    FILE *in = fopen(REGISTRY_PATH, "r");
    char u[MAX_USERNAME], p[MAX_PASSWORD], i[64];
    int po = 0;
    if (!in) {
        return -1;
    }

    while (fscanf(in, "%31s %31s %63s %d", u, p, i, &po) == 4) {
        if (strcmp(u, username) == 0) {
            if (out_password && out_password_sz > 0) {
                strncpy(out_password, p, out_password_sz - 1);
                out_password[out_password_sz - 1] = '\0';
            }
            if (out_ip) {
                strncpy(out_ip, i, 63);
                out_ip[63] = '\0';
            }
            if (out_port) {
                *out_port = po;
            }
            fclose(in);
            return 0;
        }
    }
    fclose(in);
    return 1;
}

static int lookup_user(const char *username, char *out_ip, int *out_port) {
    FILE *in = fopen(REGISTRY_PATH, "r");
    char u[MAX_USERNAME], p[MAX_PASSWORD], i[64];
    int po = 0;
    if (!in) {
        return -1;
    }

    while (fscanf(in, "%31s %31s %63s %d", u, p, i, &po) == 4) {
        if (strcmp(u, username) == 0) {
            strncpy(out_ip, i, 63);
            out_ip[63] = '\0';
            *out_port = po;
            fclose(in);
            return 0;
        }
    }
    fclose(in);
    return 1;
}

static void handle_register(int sockfd, const char *payload, const char *peer_ip) {
    char username[MAX_USERNAME], password[MAX_PASSWORD];
    int port = 0;
    char resp[256];

    if (sscanf(payload, "%31s %31s %d", username, password, &port) != 3) {
        send_message(sockfd, MSG_ERROR, "invalid register payload", 24);
        return;
    }
    if (port <= 0 || port > 65535) {
        send_message(sockfd, MSG_ERROR, "invalid port", 12);
        return;
    }

    pthread_mutex_lock(&registry_lock);
    char stored_pass[MAX_PASSWORD];
    int exists = lookup_user_with_password(username, stored_pass, sizeof(stored_pass), NULL, NULL);
    if (exists == 0 && strcmp(stored_pass, password) != 0) {
        pthread_mutex_unlock(&registry_lock);
        send_message(sockfd, MSG_ERROR, "username exists; wrong password", 30);
        return;
    }
    if (upsert_user(username, password, peer_ip, port) != 0) {
        pthread_mutex_unlock(&registry_lock);
        send_message(sockfd, MSG_ERROR, "registry update failed", 22);
        return;
    }
    pthread_mutex_unlock(&registry_lock);

    snprintf(resp, sizeof(resp), "registered %s %s:%d", username, peer_ip, port);
    send_message(sockfd, MSG_DISC_REGISTER_RESP, resp, (uint32_t)strlen(resp));
}

static void handle_lookup(int sockfd, const char *payload) {
    char username[MAX_USERNAME], ip[64];
    int port = 0;
    char resp[256];

    if (sscanf(payload, "%31s", username) != 1) {
        send_message(sockfd, MSG_ERROR, "invalid lookup payload", 22);
        return;
    }

    pthread_mutex_lock(&registry_lock);
    int rc = lookup_user(username, ip, &port);
    pthread_mutex_unlock(&registry_lock);

    if (rc == 0) {
        snprintf(resp, sizeof(resp), "%s %d", ip, port);
        send_message(sockfd, MSG_DISC_LOOKUP_RESP, resp, (uint32_t)strlen(resp));
    } else {
        send_message(sockfd, MSG_ERROR, "user not found", 14);
    }
}

static void *client_thread(void *arg) {
    int sockfd = *((int *)arg);
    free(arg);

    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    char peer_ip[64] = "0.0.0.0";
    if (getpeername(sockfd, (struct sockaddr *)&peer, &len) == 0) {
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
    }

    while (1) {
        uint32_t type = 0, payload_len = 0;
        char payload[MAX_PAYLOAD + 1];
        int rc = recv_message(sockfd, &type, payload, sizeof(payload), &payload_len);
        if (rc != 0) {
            break;
        }

        if (type == MSG_DISC_REGISTER_REQ) {
            handle_register(sockfd, payload, peer_ip);
        } else if (type == MSG_DISC_LOOKUP_REQ) {
            handle_lookup(sockfd, payload);
        } else if (type == MSG_DISCONNECT) {
            break;
        } else {
            send_message(sockfd, MSG_ERROR, "unsupported message type", 24);
        }
    }

    close(sockfd);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port\n");
        return 1;
    }
    if (ensure_registry_file() != 0) {
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("discovery_server listening on %d\n", port);
    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        int *arg = (int *)malloc(sizeof(int));
        pthread_t tid;
        if (!arg) {
            close(cfd);
            continue;
        }
        *arg = cfd;
        if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
            free(arg);
            close(cfd);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
