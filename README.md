# Embedded Linux System Monitor

This README documents the source structure, architecture, and runtime flow.

## Source structure

```text
embedded-linux-system-monior/
├── include/monitor.h             Shared structures and declarations
├── src/main.c                    Daemon, signals, socket server, shutdown
├── src/monitor.c                 Config, Linux readers, state, formatting
├── src/cli.c                     Socket client and dashboard
├── config/monitor.conf           Configuration file
├── systemd/embedded-monitor.service
├── Makefile
└── README.md
```

## Architecture

The daemon uses a producer-consumer architecture:

```text
                  embedded-monitor process
┌─────────────────────────────────────────────────────┐
│                                                     │
│ Sampling thread                  Main thread        │
│ collect_sample()                 create_server()    │
│       │                          accept()          │
│       ▼                          read command      │
│  /proc and /sys                  format response   │
│       │                          write response    │
│       └────── mutex-protected state ───────┐       │
│                                            │       │
└────────────────────────────────────────────┼───────┘
                                             │
                              Unix socket    │
                                             ▼
                                        monitor-cli
```

The sampling thread produces monitoring data. The main thread serves the latest
snapshot to CLI clients. A mutex protects the shared snapshot.

## Monitoring flow

```text
collect_sample()
    ├── cpu_read()       └── /proc/stat
    ├── memory_read()    └── /proc/meminfo
    ├── temp_read()      └── /sys/class/thermal/
    ├── network_read()   └── /proc/net/dev
    └── processes_read() └── /proc/[pid]/
```

The collected data is copied into `context.state` while `context.lock` is held.
CPU and network rates compare the current sample with the previous sample. The
sampling thread repeats once per second.

## Daemon startup flow

```text
main()
  ├── Parse command line: -c <config-file>
  ├── config_load()
  ├── Register SIGTERM, SIGINT, and SIGHUP handlers
  ├── Initialize monitor_context_t and mutex
  ├── create_server(socket_path)
  │     ├── socket()
  │     ├── unlink old socket path
  │     ├── bind()
  │     └── listen()
  ├── pthread_create() → sampling_thread()
  └── Enter client accept loop
```

## Unix socket flow

```text
embedded-monitor                         monitor-cli
────────────────                         ───────────
server = socket()                        fd = socket()
server = bind()                          connect(fd)
server = listen()  ◄─────────────────────┘

client = accept(server)

read(client, command)  ◄──── write(fd, command)

format response

write(client, response) ─────► read(fd, response)

close(client)                         close(fd)
```

The socket path is:

```text
/tmp/embedded-monitor.sock
```

`server` is the listening descriptor. `client` is the daemon's connection
descriptor for one CLI. `fd` is the CLI's connection descriptor. The server
descriptor remains open for future clients.

## Command flow

The CLI accepts exactly one command:

```text
cpu | memory | network | process | all | dashboard
```

```text
monitor-cli argv[1]
      │
      ▼
request(command, response, size)
      ├── socket()
      ├── connect()
      ├── write(command)
      ├── read(response)
      └── close()
```

The daemon uses `format_all()` for `all` and `format_section()` for the other
monitoring commands. Responses are plain text; there is no JSON protocol.

## Thread and shutdown flow

```text
Main thread                         Sampling thread
───────────                         ────────────────
accept client                       collect_sample()
read command                        sleep(1)
format response                     repeat
write response
```

Shutdown flow:

```text
SIGTERM / SIGINT / SIGHUP
            │
            ▼
stop_signal() → running = 0
            │
            ├── sampling thread exits
            ├── pthread_join(sampler)
            ├── close(server)
            ├── unlink(socket path)
            └── destroy mutex and exit
```
