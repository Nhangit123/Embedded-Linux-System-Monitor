#ifndef MONITOR_H
#define MONITOR_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#define MONITOR_SOCKET "/tmp/embedded-monitor.sock"
#define MAX_IFACES 32
#define MAX_PROCESSES 128
typedef struct
{
    char name[32];
    uint64_t rx, tx, rx_packets, tx_packets, rx_errors, tx_errors, rx_dropped, tx_dropped;
    double rx_sec, tx_sec;
} net_info_t;
typedef struct
{
    int pid;
    char name[64];
    unsigned long rss_kb;
} process_info_t;
typedef struct
{
    double cpu;
    double cores[64];
    int core_count;
    uint64_t mem_total, mem_available, swap_total, swap_free;
    double temperature;
    net_info_t net[MAX_IFACES];
    int net_count;
    process_info_t processes[MAX_PROCESSES];
    int process_count;
} monitor_state_t;
typedef struct
{
    char socket_path[108];
} monitor_config_t;
typedef struct
{
    monitor_state_t state;
    pthread_mutex_t lock;
} monitor_context_t;
int config_load(const char *, monitor_config_t *);
void collect_sample(monitor_context_t *);
void format_all(monitor_context_t *, char *, size_t);
void format_section(monitor_context_t *, const char *, char *, size_t);
#endif
