#include "monitor.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Indicate that system-monitor is running
static volatile uint8_t running = 1;

//if the program receives SIGTERM, SIGINT, or SIGHUP signals, it will set running to 0, which will cause the main loop to exit and the program to terminate gracefully.
static void stop_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void *sampling_thread(void *argument)
{
    monitor_context_t *context = argument;

    while (running) {
        //collect a sample of the system state and store it in the monitor_context_t struct
        collect_sample(context);
        sleep(1);
    }
    return NULL;
}


//open a socket and bind it to the specified path, then listen for incoming connections
static int create_server(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {0};

    if (fd < 0)
        return -1;

    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    unlink(path);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    /*
    Init config parameters 
    */
    monitor_config_t config;

    // A pointer to the config file path, defaulting to /etc/embedded-monitor.conf
    const char *config_path = "/etc/embedded-monitor.conf";
    /*
    Supported option: -c <config-file>.
    Check for unknown options and print usage information if necessary.
    */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) {
            // if option is -c, check if the next argument is a config file path
            if (i + 1 >= argc) {
                printf("Error: -c requires a configuration file\n");
                return 1;
            }
            config_path = argv[++i];
        } else {
            printf("Error: unknown option '%s'\n", argv[i]);
            printf("Usage: %s [-c config-file]\n", argv[0]);
            return -1;
        }
    }

    // Read config file and fill the config struct fields with the values from the file
    if (config_load(config_path, &config) < 0) {
        printf("Error: failed to load config path '%s'\n", config_path);
        return -1;
    }

    /*
    If recive SIGTERM, SIGINT, or SIGHUP signals call stop_signal function.
    | Signal    | Meaning                                  |
    |-----------|------------------------------------------|
    | SIGTERM   | Request the program to terminate cleanly |
    | SIGINT    | User interrupt                           |
    | SIGHUP    | Terminal disconnected or closed          | 
    */
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGHUP, stop_signal);

    /*
    Create a monitor_context_t struct that holds config, state and mutex lock.
    */
    monitor_context_t context = {0};

    //init  the mutex lock in the monitor_context_t struct
    pthread_mutex_init(&context.lock, NULL);

    //create a server socket and bind it to the specified path, then listen for incoming connections
    int server = create_server(config.socket_path);
    if (server < 0) {
        printf("Error: socket %s\n", config.socket_path);
        return -1;
    }


    //Create a new thread that will run the sampling_thread function, passing the monitor_context_t struct as an argument
    pthread_t sampler;
    pthread_create(&sampler, NULL, sampling_thread, &context);
    printf("Monitor started: socket=%s\n", config.socket_path);


    while (running) {

        /*
        wait for a client to connect to the server socket, and accept the connection
        in our case is cli program
        */ 

        int client = accept(server, NULL, NULL);
        if (client < 0) {
            printf("Error: accept failed\n");
            break;
        }

        char command[128] = {0};

        //read the command from the client socket, and store it in the command buffer
        ssize_t received = read(client, command, sizeof(command) - 1);
        if (received > 0) {
            command[strcspn(command, " \r\n")] = '\0';
            char response[8192];

            /*
            If the command is "all", call format_all function to format all the system information and store it in the response buffer.
            */
            if (!strcmp(command, "all"))
                format_all(&context, response, sizeof(response));
            /*
            If the command is "cpu", "memory", "network", or "process", call format_section function to format the corresponding section of the system information and store it in the response buffer.
            */
            else if (!strcmp(command, "cpu") || !strcmp(command, "memory") ||
                     !strcmp(command, "network") || !strcmp(command, "process"))
                format_section(&context, command, response, sizeof(response));
            else
                snprintf(response, sizeof(response), "Error: unknown command '%s'\n", command);

            //write the response to the client socket
            (void)write(client, response, strlen(response));
        }
        close(client);
    }

    running = 0;
    pthread_join(sampler, NULL);
    close(server);
    unlink(config.socket_path);
    pthread_mutex_destroy(&context.lock);
    return 0;
}
