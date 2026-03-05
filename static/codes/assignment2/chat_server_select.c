#define _POSIX_C_SOURCE 200809L

#include "net_utils.h"
#include "protocol.h"
#include "server_monitor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 100
#define BACKLOG 32
#define REGISTRY_PATH "data/registry.txt"
#define HISTORY_DIR "data/history"
#define MAX_STATUS 16

typedef struct {
    int in_use;
    int sockfd;
    int logged_in;
    char username[MAX_USERNAME];
    char status[MAX_STATUS];
} client_session_t;

static client_session_t clients[MAX_CLIENTS];

static int authenticate_user(const char *username, const char *password) {
    FILE *fp = fopen(REGISTRY_PATH, "r");
    char u[MAX_USERNAME], p[MAX_PASSWORD], ip[64];
    int port = 0;
    if (!fp) {
        return 0;
    }
    while (fscanf(fp, "%31s %31s %63s %d", u, p, ip, &port) == 4) {
        if (strcmp(u, username) == 0 && strcmp(p, password) == 0) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static int ensure_history_dir(void) {
    if (mkdir(HISTORY_DIR, 0775) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int is_valid_status(const char *status) {
    return strcmp(status, "available") == 0 || strcmp(status, "busy") == 0 || strcmp(status, "away") == 0;
}

static void now_timestamp(char *out, size_t out_sz) {
    time_t t = time(NULL);
    struct tm tm_now;
    localtime_r(&t, &tm_now);
    strftime(out, out_sz, "%Y-%m-%dT%H:%M:%S", &tm_now);
}

static void json_escape(const char *src, char *dst, size_t dst_sz) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_sz; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n' || c == '\r') {
            dst[j++] = ' ';
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static void append_history(const char *username, const char *event_type, const char *peer, const char *text) {
    char path[256];
    char ts[64];
    char e_type[64];
    char e_peer[128];
    char e_text[2048];

    if (ensure_history_dir() != 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s.jsonl", HISTORY_DIR, username);
    now_timestamp(ts, sizeof(ts));
    json_escape(event_type, e_type, sizeof(e_type));
    json_escape(peer ? peer : "-", e_peer, sizeof(e_peer));
    json_escape(text ? text : "", e_text, sizeof(e_text));

    FILE *fp = fopen(path, "a");
    if (fp) {
        fprintf(fp, "{\"timestamp\":\"%s\",\"type\":\"%s\",\"peer\":\"%s\",\"text\":\"%s\"}\n", ts, e_type, e_peer, e_text);
        fclose(fp);
    }
}

static void build_history_response(const char *username, char *out, size_t out_sz) {
    char path[256];
    out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s.jsonl", HISTORY_DIR, username);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        snprintf(out, out_sz, "(no history)");
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (strlen(out) + strlen(line) + 1 >= out_sz) {
            strncat(out, "\n...(truncated)", out_sz - strlen(out) - 1);
            break;
        }
        strncat(out, line, out_sz - strlen(out) - 1);
    }
    fclose(fp);

    if (out[0] == '\0') {
        snprintf(out, out_sz, "(no history)");
    }
}

static int is_online_username(const char *username) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in && strcmp(clients[i].username, username) == 0) {
            return 1;
        }
    }
    return 0;
}

static void release_session(int idx) {
    if (idx < 0 || idx >= MAX_CLIENTS || !clients[idx].in_use) {
        return;
    }
    close(clients[idx].sockfd);
    clients[idx].in_use = 0;
    clients[idx].sockfd = -1;
    clients[idx].logged_in = 0;
    clients[idx].username[0] = '\0';
    strncpy(clients[idx].status, "available", sizeof(clients[idx].status) - 1);
    clients[idx].status[sizeof(clients[idx].status) - 1] = '\0';
}

static void send_to_idx(int idx, uint32_t type, const char *payload) {
    if (!clients[idx].in_use) {
        return;
    }
    uint32_t len = payload ? (uint32_t)strlen(payload) : 0;
    if (send_message(clients[idx].sockfd, type, payload, len) != 0) {
        release_session(idx);
    }
}

static void build_online_user_list(char *out, size_t out_sz) {
    out[0] = '\0';
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in) {
            if (out[0] != '\0') {
                strncat(out, "\n", out_sz - strlen(out) - 1);
            }
            char row[64];
            snprintf(row, sizeof(row), "%s (%s)", clients[i].username, clients[i].status);
            strncat(out, row, out_sz - strlen(out) - 1);
        }
    }
}

static void broadcast_to_all(const char *from, const char *text) {
    char payload[MAX_PAYLOAD + 1];
    snprintf(payload, sizeof(payload), "%s %s", from, text);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in) {
            send_to_idx(i, MSG_BROADCAST_DELIVER, payload);
            append_history(clients[i].username, "broadcast_recv", from, text);
        }
    }
    append_history(from, "broadcast_send", "all", text);
}

static int send_private_to_user(const char *from, const char *to, const char *text) {
    char payload[MAX_PAYLOAD + 1];
    snprintf(payload, sizeof(payload), "%s %s", from, text);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in && strcmp(clients[i].username, to) == 0) {
            send_to_idx(i, MSG_PRIVATE_DELIVER, payload);
            append_history(clients[i].username, "private_recv", from, text);
            append_history(from, "private_send", to, text);
            return 1;
        }
    }
    return 0;
}

static void process_client_message(int idx) {
    uint32_t type = 0, payload_len = 0;
    char payload[MAX_PAYLOAD + 1];
    int rc = recv_message(clients[idx].sockfd, &type, payload, sizeof(payload), &payload_len);
    if (rc != 0) {
        release_session(idx);
        return;
    }

    if (!clients[idx].logged_in) {
        if (type != MSG_LOGIN_REQ) {
            send_to_idx(idx, MSG_ERROR, "login required");
            return;
        }
        char username[MAX_USERNAME], password[MAX_PASSWORD];
        if (sscanf(payload, "%31s %31s", username, password) != 2) {
            send_to_idx(idx, MSG_LOGIN_RESP, "FAIL invalid login payload");
            return;
        }
        if (!authenticate_user(username, password)) {
            send_to_idx(idx, MSG_LOGIN_RESP, "FAIL invalid credentials");
            return;
        }
        if (is_online_username(username)) {
            send_to_idx(idx, MSG_LOGIN_RESP, "FAIL user already online");
            return;
        }
        clients[idx].logged_in = 1;
        strncpy(clients[idx].username, username, sizeof(clients[idx].username) - 1);
        clients[idx].username[sizeof(clients[idx].username) - 1] = '\0';
        strncpy(clients[idx].status, "available", sizeof(clients[idx].status) - 1);
        clients[idx].status[sizeof(clients[idx].status) - 1] = '\0';
        send_to_idx(idx, MSG_LOGIN_RESP, "OK");
        return;
    }

    if (type == MSG_BROADCAST_REQ) {
        broadcast_to_all(clients[idx].username, payload);
        send_to_idx(idx, MSG_ACK, "broadcast sent");
    } else if (type == MSG_PRIVATE_REQ) {
        char to[MAX_USERNAME];
        char text[MAX_TEXT];
        if (sscanf(payload, "%31s %[^\n]", to, text) != 2) {
            send_to_idx(idx, MSG_ERROR, "invalid private payload");
            return;
        }
        if (send_private_to_user(clients[idx].username, to, text)) {
            send_to_idx(idx, MSG_ACK, "private sent");
        } else {
            send_to_idx(idx, MSG_ERROR, "target user offline");
        }
    } else if (type == MSG_LIST_REQ) {
        char users[MAX_PAYLOAD + 1];
        build_online_user_list(users, sizeof(users));
        send_to_idx(idx, MSG_LIST_RESP, users);
    } else if (type == MSG_STATUS_SET_REQ) {
        char new_status[MAX_STATUS];
        if (sscanf(payload, "%15s", new_status) != 1 || !is_valid_status(new_status)) {
            send_to_idx(idx, MSG_ERROR, "invalid status (use available|busy|away)");
            return;
        }
        strncpy(clients[idx].status, new_status, sizeof(clients[idx].status) - 1);
        clients[idx].status[sizeof(clients[idx].status) - 1] = '\0';
        send_to_idx(idx, MSG_STATUS, clients[idx].status);
        send_to_idx(idx, MSG_ACK, "status updated");
    } else if (type == MSG_HISTORY_REQ) {
        char history[MAX_PAYLOAD + 1];
        build_history_response(clients[idx].username, history, sizeof(history));
        send_to_idx(idx, MSG_HISTORY_RESP, history);
    } else if (type == MSG_DISCONNECT) {
        release_session(idx);
    } else {
        send_to_idx(idx, MSG_ERROR, "unsupported message type");
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port\n");
        return 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].in_use = 0;
        clients[i].sockfd = -1;
        clients[i].logged_in = 0;
        clients[i].username[0] = '\0';
        strncpy(clients[i].status, "available", sizeof(clients[i].status) - 1);
        clients[i].status[sizeof(clients[i].status) - 1] = '\0';
    }

    const char *mon_log = getenv("SERVER_MONITOR_LOG");
    const char *mon_int = getenv("SERVER_MONITOR_INTERVAL");
    int mon_interval = mon_int ? atoi(mon_int) : 5;
    if (start_server_monitor_thread(mon_log, mon_interval) != 0) {
        fprintf(stderr, "warning: failed to start monitor thread\n");
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

    printf("chat_server_select listening on %d\n", port);
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int max_fd = server_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use) {
                FD_SET(clients[i].sockfd, &readfds);
                if (clients[i].sockfd > max_fd) {
                    max_fd = clients[i].sockfd;
                }
            }
        }

        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            continue;
        }

        if (FD_ISSET(server_fd, &readfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                int assigned = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!clients[i].in_use) {
                        clients[i].in_use = 1;
                        clients[i].sockfd = cfd;
                        clients[i].logged_in = 0;
                        clients[i].username[0] = '\0';
                        strncpy(clients[i].status, "available", sizeof(clients[i].status) - 1);
                        clients[i].status[sizeof(clients[i].status) - 1] = '\0';
                        assigned = 1;
                        break;
                    }
                }
                if (!assigned) {
                    send_message(cfd, MSG_ERROR, "server full", 11);
                    close(cfd);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use && FD_ISSET(clients[i].sockfd, &readfds)) {
                process_client_message(i);
            }
        }
    }

    close(server_fd);
    stop_server_monitor_thread();
    return 0;
}
