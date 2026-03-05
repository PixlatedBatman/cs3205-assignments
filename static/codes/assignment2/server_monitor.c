#define _POSIX_C_SOURCE 200809L

#include "server_monitor.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int running;
    int interval_sec;
    pthread_t tid;
    FILE *out;
    unsigned long long prev_total_jiffies;
    unsigned long long prev_proc_jiffies;
} monitor_ctx_t;

static monitor_ctx_t g_mon = {0};

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

static void *monitor_worker(void *arg) {
    (void)arg;
    while (g_mon.running) {
        sleep((unsigned int)g_mon.interval_sec);
        if (!g_mon.running) break;

        unsigned long long total = read_total_jiffies();
        unsigned long long procj = read_proc_jiffies();
        unsigned long long vmrss = read_vmrss_kb();
        unsigned long long pss = read_pss_kb();

        unsigned long long d_total = total - g_mon.prev_total_jiffies;
        unsigned long long d_proc = procj - g_mon.prev_proc_jiffies;
        double cpu_pct = 0.0;
        if (d_total > 0) {
            cpu_pct = (100.0 * (double)d_proc) / (double)d_total;
        }

        fprintf(g_mon.out, "%lld,%.2f,%llu,%llu\n", epoch_ms(), cpu_pct, vmrss, pss);
        fflush(g_mon.out);

        g_mon.prev_total_jiffies = total;
        g_mon.prev_proc_jiffies = procj;
    }
    return NULL;
}

int start_server_monitor_thread(const char *log_path, int interval_sec) {
    if (!log_path || log_path[0] == '\0') return 0;
    if (g_mon.running) return 0;
    if (interval_sec <= 0) interval_sec = 5;

    g_mon.out = fopen(log_path, "w");
    if (!g_mon.out) return -1;
    fprintf(g_mon.out, "epoch_ms,cpu_percent,vmrss_kb,pss_kb\n");
    fflush(g_mon.out);

    g_mon.interval_sec = interval_sec;
    g_mon.prev_total_jiffies = read_total_jiffies();
    g_mon.prev_proc_jiffies = read_proc_jiffies();
    g_mon.running = 1;

    if (pthread_create(&g_mon.tid, NULL, monitor_worker, NULL) != 0) {
        fclose(g_mon.out);
        g_mon.out = NULL;
        g_mon.running = 0;
        return -1;
    }
    return 0;
}

void stop_server_monitor_thread(void) {
    if (!g_mon.running) return;
    g_mon.running = 0;
    pthread_join(g_mon.tid, NULL);
    if (g_mon.out) {
        fclose(g_mon.out);
        g_mon.out = NULL;
    }
}
