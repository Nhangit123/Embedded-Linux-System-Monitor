CC = gcc
CFLAGS = -Iinclude
LIBS = -pthread

all: embedded-monitor monitor-cli

embedded-monitor: src/main.o src/monitor.o
	$(CC) $^ $(LIBS) -o $@

monitor-cli: src/cli.o
	$(CC) $^ $(LIBS) -o $@

src/main.o: src/main.c include/monitor.h
	$(CC) $(CFLAGS) -c src/main.c -o src/main.o

src/monitor.o: src/monitor.c include/monitor.h
	$(CC) $(CFLAGS) -c src/monitor.c -o src/monitor.o

src/cli.o: src/cli.c include/monitor.h
	$(CC) $(CFLAGS) -c src/cli.c -o src/cli.o

clean:
	rm -f src/*.o embedded-monitor monitor-cli

install: all
	install -D -m 755 embedded-monitor /usr/sbin/embedded-monitor
	install -D -m 755 monitor-cli /usr/bin/monitor-cli
	install -D -m 644 config/monitor.conf /etc/embedded-monitor.conf
	install -D -m 644 systemd/embedded-monitor.service /etc/systemd/system/embedded-monitor.service

.PHONY: all clean install
