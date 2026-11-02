#include "monitor.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile uint8_t dashboard_running = 1;
static void stop_dashboard(int signal_number)
{
    (void)signal_number;
    dashboard_running = 0;
}

static int valid_command(const char *command)
{
    return !strcmp(command, "cpu") ||
           !strcmp(command, "memory") ||
           !strcmp(command, "network") ||
           !strcmp(command, "process") ||
           !strcmp(command, "all") ||
           !strcmp(command, "dashboard");
}

static int request(const char *command, char *response, size_t size)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {0};
    if (fd < 0)
        return -1;
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", MONITOR_SOCKET);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        close(fd);
        return -1;
    }
    (void)write(fd, command, strlen(command));
    ssize_t received = read(fd, response, size - 1);
    close(fd);
    if (received < 0)
        return -1;
    response[received] = '\0';
    return 0;
}

static int dashboard(void)
{
    char response[8192];

    // if the program receives SIGTERM, SIGINT signals, run the stop_dashboard function.
    signal(SIGINT, stop_dashboard);
    signal(SIGTERM, stop_dashboard);
    printf("\033[?25l");
    while (dashboard_running)
    {
        if (request("all", response, sizeof(response)) < 0)
        {
            printf("Cannot connect to %s: %s\n", MONITOR_SOCKET, strerror(errno));
            printf("\033[?25h");
            return 1;
        }
        printf("\033[H\033[2J");
        printf("\033[1;36mEmbedded Linux System Monitor\033[0m\n");
        printf("=============================\n\n");
        fputs(response, stdout);
        printf("\nPress Ctrl-C to exit | refresh: 1 second\n");
        fflush(stdout);
        sleep(1);
    }
    printf("\033[?25h\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s cpu|memory|network|process|all|dashboard\n", argv[0]);
        return 2;
    }
    if (!valid_command(argv[1]))
    {
        printf("Error: unknown command '%s'\n", argv[1]);
        printf("Usage: %s cpu|memory|network|process|all|dashboard\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "dashboard"))
        return dashboard();
    char response[8192];
    if (request(argv[1], response, sizeof(response)) < 0)
    {
        printf("Cannot connect to %s: %s\n", MONITOR_SOCKET, strerror(errno));
        return 1;
    }
    fputs(response, stdout);
    return 0;
}
