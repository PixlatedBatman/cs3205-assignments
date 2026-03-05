#ifndef SERVER_MONITOR_H
#define SERVER_MONITOR_H

int start_server_monitor_thread(const char *log_path, int interval_sec);
void stop_server_monitor_thread(void);

#endif
