#define _POSIX_C_SOURCE 200809L

#include "net_utils.h"
#include "protocol.h"

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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 100
#define BACKLOG 32
#define REGISTRY_PATH "data/registry.txt"
#define HISTORY_DIR "data/history"
#define MAX_STATUS 16

typedef struct {
    uint32_t client_idx;
    uint32_t type;
    uint32_t len;
} parent_event_header_t;

typedef struct {
    int in_use;
    int sockfd;
    int pipe_read_fd;
    pid_t child_pid;
    int logged_in;
    char username[MAX_USERNAME];
    char status[MAX_STATUS];
} client_session_t;

static client_session_t clients[MAX_CLIENTS];
static FILE *monitor_fp = NULL;
static time_t monitor_last = 0;

static unsigned long long mon_prev_total = 0;
static unsigned long long mon_prev_proc = 0;
static int mon_interval = 5;

static unsigned long long read_total_jiffies(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0;
    char line[512];
    unsigned long long vals[10] = {0};
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
           &vals[0], &vals[1], &vals[2], &vals[3], &vals[4],
           &vals[5], &vals[6], &vals[7], &vals[8], &vals[9]);
    unsigned long long sum = 0;
    for (int i = 0; i < 10; i++) sum += vals[i];
    return sum;
}

static unsigned long long read_proc_jiffies(void) {
    FILE *fp = fopen("/proc/self/stat", "r");
    if (!fp) return 0;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    char *rp = strrchr(buf, ')');
    if (!rp) return 0;
    char *rest = rp + 2;
    unsigned long long utime = 0, stime = 0;
    int idx = 1;
    char *tok = strtok(rest, " ");
    while (tok) {
        if (idx == 12) utime = strtoull(tok, NULL, 10);
        if (idx == 13) {
            stime = strtoull(tok, NULL, 10);
            break;
        }
        idx++;
        tok = strtok(NULL, " ");
    }
    return utime + stime;
}

static unsigned long long read_vmrss_kb(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long long v = 0;
        if (sscanf(line, "VmRSS: %llu kB", &v) == 1) {
            fclose(fp);
            return v;
        }
    }
    fclose(fp);
    return 0;
}

static unsigned long long read_pss_kb(void) {
    FILE *fp = fopen("/proc/self/smaps_rollup", "r");
    if (!fp) return 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long long v = 0;
        if (sscanf(line, "Pss: %llu kB", &v) == 1) {
            fclose(fp);
            return v;
        }
    }
    fclose(fp);
    return 0;
}

static long long epoch_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static void monitor_init_from_env(void) {
    const char *path = getenv("SERVER_MONITOR_LOG");
    const char *ival = getenv("SERVER_MONITOR_INTERVAL");
    if (ival) {
        int v = atoi(ival);
        if (v > 0) mon_interval = v;
    }
    if (path && path[0] != '\0') {
        monitor_fp = fopen(path, "w");
        if (monitor_fp) {
            fprintf(monitor_fp, "epoch_ms,cpu_percent,vmrss_kb,pss_kb\n");
            fflush(monitor_fp);
            mon_prev_total = read_total_jiffies();
            mon_prev_proc = read_proc_jiffies();
            monitor_last = time(NULL);
        }
    }
}

static void monitor_tick_if_due(void) {
    if (!monitor_fp) return;
    time_t now = time(NULL);
    if (now - monitor_last < mon_interval) return;

    unsigned long long total = read_total_jiffies();
    unsigned long long procj = read_proc_jiffies();
    unsigned long long vmrss = read_vmrss_kb();
    unsigned long long pss = read_pss_kb();
    unsigned long long d_total = total - mon_prev_total;
    unsigned long long d_proc = procj - mon_prev_proc;
    double cpu_pct = 0.0;
    if (d_total > 0) cpu_pct = (100.0 * (double)d_proc) / (double)d_total;

    fprintf(monitor_fp, "%lld,%.2f,%llu,%llu\n", epoch_ms(), cpu_pct, vmrss, pss);
    fflush(monitor_fp);
    mon_prev_total = total;
    mon_prev_proc = procj;
    monitor_last = now;
}

static int write_all_fd(int fd, const void *buf, int len) {
    const char *p = (const char *)buf;
    int done = 0;
    while (done < len) {
        int n = (int)write(fd, p + done, (size_t)(len - done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        done += n;
    }
    return 0;
}

static int read_all_fd(int fd, void *buf, int len) {
    char *p = (char *)buf;
    int done = 0;
    while (done < len) {
        int n = (int)read(fd, p + done, (size_t)(len - done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return 1;
        }
        done += n;
    }
    return 0;
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
    monitor_init_from_env();
    return 0;
}

static void release_session(int idx) {
    if (idx < 0 || idx >= MAX_CLIENTS || !clients[idx].in_use) {
        return;
    }
    close(clients[idx].sockfd);
    close(clients[idx].pipe_read_fd);
    clients[idx].in_use = 0;
    clients[idx].sockfd = -1;
    clients[idx].pipe_read_fd = -1;
    clients[idx].child_pid = -1;
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

static int child_forward_loop(int client_sockfd, int write_fd, int client_idx) {
    uint32_t type = 0, payload_len = 0;
    char payload[MAX_PAYLOAD + 1];

    while (1) {
        int rc = recv_message(client_sockfd, &type, payload, sizeof(payload), &payload_len);
        if (rc != 0) {
            type = MSG_DISCONNECT;
            payload_len = 0;
        }

        parent_event_header_t eh;
        eh.client_idx = htonl((uint32_t)client_idx);
        eh.type = htonl(type);
        eh.len = htonl(payload_len);

        if (write_all_fd(write_fd, &eh, (int)sizeof(eh)) != 0) {
            break;
        }
        if (payload_len > 0 && write_all_fd(write_fd, payload, (int)payload_len) != 0) {
            break;
        }
        if (type == MSG_DISCONNECT) {
            break;
        }
    }
    return 0;
}

static void handle_sigchld(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static void process_child_event(int pipe_fd) {
    parent_event_header_t eh;
    int rc = read_all_fd(pipe_fd, &eh, (int)sizeof(eh));
    if (rc != 0) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use && clients[i].pipe_read_fd == pipe_fd) {
                release_session(i);
                break;
            }
        }
        return;
    }

    int idx = (int)ntohl(eh.client_idx);
    uint32_t type = ntohl(eh.type);
    uint32_t len = ntohl(eh.len);
    if (idx < 0 || idx >= MAX_CLIENTS || !clients[idx].in_use || len > MAX_PAYLOAD) {
        return;
    }

    char payload[MAX_PAYLOAD + 1];
    if (len > 0) {
        rc = read_all_fd(pipe_fd, payload, (int)len);
        if (rc != 0) {
            release_session(idx);
            return;
        }
    }
    payload[len] = '\0';

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
        clients[i].pipe_read_fd = -1;
        clients[i].child_pid = -1;
        clients[i].logged_in = 0;
        clients[i].username[0] = '\0';
        strncpy(clients[i].status, "available", sizeof(clients[i].status) - 1);
        clients[i].status[sizeof(clients[i].status) - 1] = '\0';
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigchld;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

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

    printf("chat_server_fork listening on %d\n", port);
    while (1) {
        monitor_tick_if_due();
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int max_fd = server_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use) {
                FD_SET(clients[i].pipe_read_fd, &readfds);
                if (clients[i].pipe_read_fd > max_fd) {
                    max_fd = clients[i].pipe_read_fd;
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
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!clients[i].in_use) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    send_message(cfd, MSG_ERROR, "server full", 11);
                    close(cfd);
                } else {
                    int pipefd[2];
                    if (pipe(pipefd) != 0) {
                        close(cfd);
                    } else {
                        pid_t pid = fork();
                        if (pid < 0) {
                            close(pipefd[0]);
                            close(pipefd[1]);
                            close(cfd);
                        } else if (pid == 0) {
                            close(server_fd);
                            close(pipefd[0]);
                            child_forward_loop(cfd, pipefd[1], slot);
                            close(cfd);
                            close(pipefd[1]);
                            _exit(0);
                        } else {
                            close(pipefd[1]);
                            clients[slot].in_use = 1;
                            clients[slot].sockfd = cfd;
                            clients[slot].pipe_read_fd = pipefd[0];
                            clients[slot].child_pid = pid;
                            clients[slot].logged_in = 0;
                            clients[slot].username[0] = '\0';
                            strncpy(clients[slot].status, "available", sizeof(clients[slot].status) - 1);
                            clients[slot].status[sizeof(clients[slot].status) - 1] = '\0';
                        }
                    }
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use && FD_ISSET(clients[i].pipe_read_fd, &readfds)) {
                process_child_event(clients[i].pipe_read_fd);
            }
        }
    }

    close(server_fd);
    if (monitor_fp) fclose(monitor_fp);
    return 0;
}
