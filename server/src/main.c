/**
 * @file main.c
 * @brief Server entry point.
 *
 * This file handles server configuration, socket initialization, and the main
 * connection acceptance loop. It delegates client handling to separate threads.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include "client.h"
#include "match.h"
#include "game.h"
#include "logging.h"
#include "config.h"

#define BACKLOG 10

/* Global configuration limits (modified via command line args) */
int max_rooms = -1;
int max_players = -1;

/**
 * @brief Main function.
 *
 * Steps:
 * 1. Initializes logging subsystem.
 * 2. Parses command line arguments for IP, Port, and Limits.
 * 3. Binds and listens on the TCP socket.
 * 4. Enters an infinite loop to accept incoming connections.
 * 5. Spawns a dedicated thread for each client.
 */
int main(int argc, char *argv[]) {
    init_logging();
    struct in_addr bind_addr;
    bind_addr.s_addr = htonl(INADDR_ANY);
    int port = DEFAULT_PORT;

    /* Parse Command Line Arguments */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "ip=", 3) == 0) {
            const char *ip_str = argv[i] + 3;
            if (strcmp(ip_str, "any") != 0 && strcmp(ip_str, "0.0.0.0") != 0) {
                if (inet_pton(AF_INET, ip_str, &bind_addr) != 1) return EXIT_FAILURE;
            }
        } 
        else if (strncmp(argv[i], "port=", 5) == 0) port = atoi(argv[i] + 5);
        else if (strncmp(argv[i], "rooms=", 6) == 0) max_rooms = atoi(argv[i] + 6);
        else if (strncmp(argv[i], "players=", 8) == 0) max_players = atoi(argv[i] + 8);
    }
    
    /* Socket Setup */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = bind_addr;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    if (listen(srv, BACKLOG) < 0) return 1;

    log_printf("Server listening on port %d - Max Rooms: %d, Max Players: %d (-1: unlimited)\n", port, max_rooms, max_players);

    /* Connection Acceptance Loop */
    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int csock = accept(srv, (struct sockaddr *)&cliaddr, &clilen);
        if (csock < 0) continue;

        /* Allocate Client Structure */
        Client *c = calloc(1, sizeof(Client));
        if (!c) { close(csock); continue; }

        /* Initialize Client State */
        c->sock = csock;
        c->color = -1;
        c->paired = 0;
        c->match = NULL;
        c->error_count = 0;
        c->is_counted = 0;
        c->state = STATE_HANDSHAKE;
        pthread_mutex_init(&c->lock, NULL);

        char addrbuf[ADDR_LEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, addrbuf, sizeof(addrbuf));
        snprintf(c->client_addr, sizeof(c->client_addr), "%s:%u", addrbuf, ntohs(cliaddr.sin_port));

        /* Spawn Worker Thread */
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_worker, c) != 0) { close(csock); free(c); }
        else pthread_detach(tid);
    }
    close_logging();
    return 0;
}