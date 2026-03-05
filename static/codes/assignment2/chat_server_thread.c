#define _POSIX_C_SOURCE 200809L

#include "net_utils.h"
#include "protocol.h"
#include "server_monitor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    pthread_t thread_id;
    pthread_mutex_t send_lock;
} client_session_t;

static client_session_t clients[MAX_CLIENTS];
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t history_lock = PTHREAD_MUTEX_INITIALIZER;

static int safe_send(client_session_t *c, uint32_t type, const char *payload) {
    uint32_t len = payload ? (uint32_t)strlen(payload) : 0;
    pthread_mutex_lock(&c->send_lock);
    int rc = send_message(c->sockfd, type, payload, len);
    pthread_mutex_unlock(&c->send_lock);
    return rc;
}

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

    pthread_mutex_lock(&history_lock);
    FILE *fp = fopen(path, "a");
    if (fp) {
        fprintf(fp, "{\"timestamp\":\"%s\",\"type\":\"%s\",\"peer\":\"%s\",\"text\":\"%s\"}\n", ts, e_type, e_peer, e_text);
        fclose(fp);
    }
    pthread_mutex_unlock(&history_lock);
}

static void build_history_response(const char *username, char *out, size_t out_sz) {
    char path[256];
    out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s.jsonl", HISTORY_DIR, username);

    pthread_mutex_lock(&history_lock);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        pthread_mutex_unlock(&history_lock);
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
    pthread_mutex_unlock(&history_lock);

    if (out[0] == '\0') {
        snprintf(out, out_sz, "(no history)");
    }
}

static int is_online_username(const char *username) {
    int found = 0;
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in && strcmp(clients[i].username, username) == 0) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    return found;
}

static void build_online_user_list(char *out, size_t out_sz) {
    out[0] = '\0';
    pthread_mutex_lock(&clients_lock);
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
    pthread_mutex_unlock(&clients_lock);
}

static void broadcast_to_all(const char *from, const char *text) {
    char payload[MAX_PAYLOAD + 1];
    snprintf(payload, sizeof(payload), "%s %s", from, text);

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in) {
            safe_send(&clients[i], MSG_BROADCAST_DELIVER, payload);
            append_history(clients[i].username, "broadcast_recv", from, text);
        }
    }
    pthread_mutex_unlock(&clients_lock);
    append_history(from, "broadcast_send", "all", text);
}

static int send_private_to_user(const char *from, const char *to, const char *text) {
    char payload[MAX_PAYLOAD + 1];
    snprintf(payload, sizeof(payload), "%s %s", from, text);
    int delivered = 0;

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in && strcmp(clients[i].username, to) == 0) {
            safe_send(&clients[i], MSG_PRIVATE_DELIVER, payload);
            append_history(clients[i].username, "private_recv", from, text);
            delivered = 1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    if (delivered) {
        append_history(from, "private_send", to, text);
    }
    return delivered;
}

static void release_session(client_session_t *self) {
    pthread_mutex_lock(&clients_lock);
    self->in_use = 0;
    self->logged_in = 0;
    self->username[0] = '\0';
    strncpy(self->status, "available", sizeof(self->status) - 1);
    self->status[sizeof(self->status) - 1] = '\0';
    int fd = self->sockfd;
    self->sockfd = -1;
    pthread_mutex_unlock(&clients_lock);
    if (fd >= 0) {
        close(fd);
    }
}

static void *client_worker(void *arg) {
    client_session_t *self = (client_session_t *)arg;
    uint32_t type = 0, payload_len = 0;
    char payload[MAX_PAYLOAD + 1];

    while (1) {
        int rc = recv_message(self->sockfd, &type, payload, sizeof(payload), &payload_len);
        if (rc != 0) {
            break;
        }

        if (!self->logged_in) {
            if (type != MSG_LOGIN_REQ) {
                safe_send(self, MSG_ERROR, "login required");
                continue;
            }

            char username[MAX_USERNAME], password[MAX_PASSWORD];
            if (sscanf(payload, "%31s %31s", username, password) != 2) {
                safe_send(self, MSG_LOGIN_RESP, "FAIL invalid login payload");
                continue;
            }
            if (!authenticate_user(username, password)) {
                safe_send(self, MSG_LOGIN_RESP, "FAIL invalid credentials");
                continue;
            }
            if (is_online_username(username)) {
                safe_send(self, MSG_LOGIN_RESP, "FAIL user already online");
                continue;
            }

            pthread_mutex_lock(&clients_lock);
            self->logged_in = 1;
            strncpy(self->username, username, sizeof(self->username) - 1);
            self->username[sizeof(self->username) - 1] = '\0';
            strncpy(self->status, "available", sizeof(self->status) - 1);
            self->status[sizeof(self->status) - 1] = '\0';
            pthread_mutex_unlock(&clients_lock);

            safe_send(self, MSG_LOGIN_RESP, "OK");
            continue;
        }

        if (type == MSG_BROADCAST_REQ) {
            broadcast_to_all(self->username, payload);
            safe_send(self, MSG_ACK, "broadcast sent");
        } else if (type == MSG_PRIVATE_REQ) {
            char to[MAX_USERNAME];
            char text[MAX_TEXT];
            if (sscanf(payload, "%31s %[^\n]", to, text) != 2) {
                safe_send(self, MSG_ERROR, "invalid private payload");
                continue;
            }
            if (send_private_to_user(self->username, to, text)) {
                safe_send(self, MSG_ACK, "private sent");
            } else {
                safe_send(self, MSG_ERROR, "target user offline");
            }
        } else if (type == MSG_LIST_REQ) {
            char users[MAX_PAYLOAD + 1];
            build_online_user_list(users, sizeof(users));
            safe_send(self, MSG_LIST_RESP, users);
        } else if (type == MSG_STATUS_SET_REQ) {
            char new_status[MAX_STATUS];
            if (sscanf(payload, "%15s", new_status) != 1 || !is_valid_status(new_status)) {
                safe_send(self, MSG_ERROR, "invalid status (use available|busy|away)");
                continue;
            }
            pthread_mutex_lock(&clients_lock);
            strncpy(self->status, new_status, sizeof(self->status) - 1);
            self->status[sizeof(self->status) - 1] = '\0';
            pthread_mutex_unlock(&clients_lock);
            safe_send(self, MSG_STATUS, self->status);
            safe_send(self, MSG_ACK, "status updated");
        } else if (type == MSG_HISTORY_REQ) {
            char history[MAX_PAYLOAD + 1];
            build_history_response(self->username, history, sizeof(history));
            safe_send(self, MSG_HISTORY_RESP, history);
        } else if (type == MSG_DISCONNECT) {
            break;
        } else {
            safe_send(self, MSG_ERROR, "unsupported message type");
        }
    }

    release_session(self);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].in_use = 0;
        clients[i].sockfd = -1;
        clients[i].logged_in = 0;
        clients[i].username[0] = '\0';
        strncpy(clients[i].status, "available", sizeof(clients[i].status) - 1);
        clients[i].status[sizeof(clients[i].status) - 1] = '\0';
        pthread_mutex_init(&clients[i].send_lock, NULL);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port\n");
        return 1;
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

    printf("chat_server_thread listening on %d\n", port);
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

        pthread_mutex_lock(&clients_lock);
        client_session_t *slot = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].in_use) {
                slot = &clients[i];
                slot->in_use = 1;
                slot->sockfd = cfd;
                slot->logged_in = 0;
                slot->username[0] = '\0';
                strncpy(slot->status, "available", sizeof(slot->status) - 1);
                slot->status[sizeof(slot->status) - 1] = '\0';
                break;
            }
        }
        pthread_mutex_unlock(&clients_lock);

        if (!slot) {
            send_message(cfd, MSG_ERROR, "server full", 11);
            close(cfd);
            continue;
        }

        if (pthread_create(&slot->thread_id, NULL, client_worker, slot) != 0) {
            perror("pthread_create");
            release_session(slot);
            continue;
        }
        pthread_detach(slot->thread_id);
    }

    close(server_fd);
    stop_server_monitor_thread();
    return 0;
}
