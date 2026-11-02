#include "monitor.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

typedef struct
{
    unsigned long long total, idle;
} ticks_t;
static ticks_t old_cpu[65];
static int have_cpu;
static uint64_t old_rx[MAX_IFACES], old_tx[MAX_IFACES];
static struct timespec old_net;
static int have_net;
static double pct(unsigned long long x, unsigned long long y) { return y ? 100.0 * (double)x / (double)y : 0.0; }

int config_load(const char *path, monitor_config_t *c)
{
    memset(c, 0, sizeof(*c));

    /* copy default values to the config struct fields 
    #define MONITOR_SOCKET "/tmp/embedded-monitor.sock"
    */
    snprintf(c->socket_path, sizeof(c->socket_path), "%s", MONITOR_SOCKET);

    //open the config file for reading and fill the config struct fields with the values from the file
    FILE *f = fopen(path, "r");
    if (!f)
    {
        printf("Error: failed to open config file '%s'\n", path);
        return -1;
    }
        
    char line[256], key[80], value[160];
    while (fgets(line, sizeof(line), f))
        if (line[0] != '#' && sscanf(line, " %79[^=]= %159s", key, value) == 2)
        {
            if (!strcmp(key, "socket_path"))
                snprintf(c->socket_path, sizeof(c->socket_path), "%s", value);
        }
    fclose(f);
    return 0;
}

static void cpu_read(monitor_state_t *s)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return;
    char line[512], label[32];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < 65)
    {
        if (strncmp(line, "cpu", 3) || sscanf(line, "%31s", label) != 1)
            continue;
        unsigned long long a, b, c, d, e, g, h, i, j, k;
        int count = sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &a, &b, &c, &d, &e, &g, &h, &i, &j, &k);
        if (count < 4)
            continue;
        ticks_t now = {a + b + c + d + e + g + h + i + j + k, d + e};
        if (have_cpu)
        {
            unsigned long long total = now.total - old_cpu[n].total, idle = now.idle - old_cpu[n].idle;
            double v = pct(total - (idle > total ? total : idle), total);
            if (n == 0)
                s->cpu = v;
            else
                s->cores[n - 1] = v;
        }
        old_cpu[n++] = now;
    }
    s->core_count = n > 0 ? n - 1 : 0;
    have_cpu = 1;
    fclose(f);
}
static void memory_read(monitor_state_t *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return;
    char key[64], unit[16];
    unsigned long long v;
    while (fscanf(f, "%63[^:]: %llu %15s\n", key, &v, unit) >= 2)
    {
        if (!strcmp(key, "MemTotal"))
            s->mem_total = v;
        else if (!strcmp(key, "MemAvailable"))
            s->mem_available = v;
        else if (!strcmp(key, "SwapTotal"))
            s->swap_total = v;
        else if (!strcmp(key, "SwapFree"))
            s->swap_free = v;
    }
    fclose(f);
}
static void temp_read(monitor_state_t *s)
{
    DIR *d = opendir("/sys/class/thermal");
    s->temperature = -1;
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (strncmp(e->d_name, "thermal_zone", 11))
            continue;
        char p[256];
        snprintf(p, sizeof(p), "/sys/class/thermal/%s/temp", e->d_name);
        FILE *f = fopen(p, "r");
        long v;
        if (f && fscanf(f, "%ld", &v) == 1)
        {
            s->temperature = v / 1000.0;
            fclose(f);
            break;
        }
        if (f)
            fclose(f);
    }
    closedir(d);
}
static void network_read(monitor_state_t *s)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = have_net ? (now.tv_sec - old_net.tv_sec) + (now.tv_nsec - old_net.tv_nsec) / 1e9 : 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < MAX_IFACES)
    {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = 0;
        char *name = line;
        while (*name == ' ')
            name++;
        net_info_t *x = &s->net[n];
        snprintf(x->name, sizeof(x->name), "%s", name);
        unsigned long long v[16] = {0};
        if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) < 10)
            continue;
        x->rx = v[0];
        x->rx_packets = v[1];
        x->rx_errors = v[2];
        x->rx_dropped = v[3];
        x->tx = v[8];
        x->tx_packets = v[9];
        x->tx_errors = v[10];
        x->tx_dropped = v[11];
        if (have_net && dt > 0)
        {
            x->rx_sec = (x->rx - old_rx[n]) / dt;
            x->tx_sec = (x->tx - old_tx[n]) / dt;
        }
        old_rx[n] = x->rx;
        old_tx[n] = x->tx;
        n++;
    }
    s->net_count = n;
    old_net = now;
    have_net = 1;
    fclose(f);
}
static void processes_read(monitor_state_t *s)
{
    DIR *d = opendir("/proc");
    if (!d)
        return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < MAX_PROCESSES)
    {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0)
            continue;
        char p[128], name[64] = {0};
        FILE *f;
        snprintf(p, sizeof(p), "/proc/%ld/stat", pid);
        f = fopen(p, "r");
        if (!f)
            continue;
        char state;
        unsigned long long ut, st;
        int ok = fscanf(f, "%*d (%63[^)]) %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", name, &state, &ut, &st) == 4;
        fclose(f);
        if (!ok)
            continue;
        unsigned long pages = 0;
        snprintf(p, sizeof(p), "/proc/%ld/statm", pid);
        f = fopen(p, "r");
        if (f)
        {
            fscanf(f, "%*lu %lu", &pages);
            fclose(f);
        }
        s->processes[n].pid = (int)pid;
        snprintf(s->processes[n].name, sizeof(s->processes[n].name), "%s", name);
        s->processes[n].rss_kb = pages * (unsigned long)(sysconf(_SC_PAGESIZE) / 1024);
        n++;
    }
    s->process_count = n;
    closedir(d);
}

// Collect a sample of the system state and store it in the monitor_context_t struct
void collect_sample(monitor_context_t *c)
{
    monitor_state_t next = {0};
    cpu_read(&next);
    memory_read(&next);
    temp_read(&next);
    network_read(&next);
    processes_read(&next);
    pthread_mutex_lock(&c->lock);
    c->state = next;
    pthread_mutex_unlock(&c->lock);
}
static void add(char *o, size_t z, int *p, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    if ((size_t)*p < z)
        *p += vsnprintf(o + *p, z - (size_t)*p, f, a);
    va_end(a);
}
void format_all(monitor_context_t *c, char *o, size_t z)
{
    monitor_state_t s;

    //lock the mutex lock in the monitor_context_t struct to prevent other threads from accessing the state while we are reading it
    pthread_mutex_lock(&c->lock);
    s = c->state;
    pthread_mutex_unlock(&c->lock);
    int p = 0;
    add(o, z, &p, "CPU total: %.2f%%\n", s.cpu);
    for (int i = 0; i < s.core_count; i++)
        add(o, z, &p, "CPU core %d: %.2f%%\n", i, s.cores[i]);
    add(o, z, &p, "Memory: %llu/%llu KB (%.2f%%)\n", (unsigned long long)(s.mem_total - s.mem_available), (unsigned long long)s.mem_total, pct(s.mem_total - s.mem_available, s.mem_total));
    add(o, z, &p, "Swap: %llu/%llu KB\n", (unsigned long long)(s.swap_total - s.swap_free), (unsigned long long)s.swap_total);
    add(o, z, &p, "Temperature: %.2f C\n", s.temperature);
    add(o, z, &p, "Network:\n");
    for (int i = 0; i < s.net_count; i++)
        add(o, z, &p, "  %s RX %.0f B/s TX %.0f B/s packets RX=%llu TX=%llu errors RX=%llu TX=%llu dropped RX=%llu TX=%llu\n",
            s.net[i].name, s.net[i].rx_sec, s.net[i].tx_sec,
            (unsigned long long)s.net[i].rx_packets,
            (unsigned long long)s.net[i].tx_packets,
            (unsigned long long)s.net[i].rx_errors,
            (unsigned long long)s.net[i].tx_errors,
            (unsigned long long)s.net[i].rx_dropped,
            (unsigned long long)s.net[i].tx_dropped);
    add(o, z, &p, "Processes: %d\n", s.process_count);
    if (z)
        o[z - 1] = 0;
}
void format_section(monitor_context_t *c, const char *sec, char *o, size_t z)
{
    monitor_state_t s;
    pthread_mutex_lock(&c->lock);
    s = c->state;
    pthread_mutex_unlock(&c->lock);
    int p = 0;
    if (!strcmp(sec, "cpu"))
    {
        p += snprintf(o + p, z > (size_t)p ? z - p : 0, "CPU total: %.2f%%\n", s.cpu);
        for (int i = 0; i < s.core_count; i++)
            p += snprintf(o + p, z > (size_t)p ? z - p : 0, "CPU core %d: %.2f%%\n", i, s.cores[i]);
    }
    else if (!strcmp(sec, "memory"))
        snprintf(o, z, "Memory: %llu/%llu KB (%.2f%%)\nSwap: %llu/%llu KB\n", (unsigned long long)(s.mem_total - s.mem_available), (unsigned long long)s.mem_total, pct(s.mem_total - s.mem_available, s.mem_total), (unsigned long long)(s.swap_total - s.swap_free), (unsigned long long)s.swap_total);
    else if (!strcmp(sec, "network"))
    {
        for (int i = 0; i < s.net_count; i++)
            p += snprintf(o + p, z > (size_t)p ? z - p : 0, "%s RX %.0f B/s TX %.0f B/s packets RX=%llu TX=%llu errors RX=%llu TX=%llu dropped RX=%llu TX=%llu\n",
                          s.net[i].name, s.net[i].rx_sec, s.net[i].tx_sec,
                          (unsigned long long)s.net[i].rx_packets,
                          (unsigned long long)s.net[i].tx_packets,
                          (unsigned long long)s.net[i].rx_errors,
                          (unsigned long long)s.net[i].tx_errors,
                          (unsigned long long)s.net[i].rx_dropped,
                          (unsigned long long)s.net[i].tx_dropped);
    }
    else if (!strcmp(sec, "process"))
    {
        for (int i = 0; i < s.process_count && i < 20; i++)
            p += snprintf(o + p, z > (size_t)p ? z - p : 0, "%d %-24s %lu KB\n", s.processes[i].pid, s.processes[i].name, s.processes[i].rss_kb);
    }
    else
        format_all(c, o, z);
}
