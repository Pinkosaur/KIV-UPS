/* main.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <errno.h>
#include <pthread.h>

#include "client.h"
#include "match.h"
#include "game.h"
#include "logging.h"

#define DEFAULT_PORT 10001
#define BACKLOG 10

/* Global configuration limits (exported to other modules) */
int max_rooms = -1;    /* -1 = unlimited */
int max_players = -1;  /* -1 = unlimited */

int main(int argc, char *argv[]) {
    struct in_addr bind_addr;
    bind_addr.s_addr = htonl(INADDR_ANY);
    int port = DEFAULT_PORT;

    /* Argument parsing ... */
    if (argc > 5) {
        fprintf(stderr, "Usage: %s [IP|any] [PORT] [MAX_ROOMS] [MAX_PLAYERS]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2) {
        const char *iparg = argv[1];
        if (strcmp(iparg, "any") == 0 || strcmp(iparg, "0.0.0.0") == 0) {
            bind_addr.s_addr = htonl(INADDR_ANY);
        } else {
            if (inet_pton(AF_INET, iparg, &bind_addr) != 1) {
                fprintf(stderr, "Invalid IP address '%s'\n", iparg);
                return EXIT_FAILURE;
            }
        }
    }

    if (argc >= 3) {
        port = atoi(argv[2]);
    }

    if (argc >= 4) {
        max_rooms = atoi(argv[3]);
    }

    if (argc >= 5) {
        max_players = atoi(argv[4]);
    }

    list_local_interfaces();
    putchar('\n');

    char addrbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &bind_addr, addrbuf, sizeof(addrbuf));
    log_printf("Server configuration:\n");
    log_printf("  bind IP       : %s\n", addrbuf);
    log_printf("  port          : %d\n", port);
    log_printf("  max rooms     : %s\n", (max_rooms == -1) ? "unlimited" : argv[3]);
    log_printf("  max players   : %s\n", (max_players == -1) ? "unlimited" : argv[4]);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = bind_addr;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); return 1;
    }

    log_printf("Server listening on %s:%d\n", addrbuf, port);

    while (1) {
        log_printf("[MAIN] Starting iteration of the infinite loop in main.\n");
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int csock = accept(srv, (struct sockaddr *)&cliaddr, &clilen);
        if (csock < 0) continue;

        Client *c = calloc(1, sizeof(Client));
        if (!c) { log_printf("[MAIN] Failed to allocate client. closing sock and continuing.\n"); close(csock); continue; }
        log_printf("[MAIN] Allocated client.\n");

        c->sock = csock;
        c->color = -1;
        c->paired = 0;
        c->match = NULL;
        c->seq = -1; 
        c->error_count = 0;
        log_printf("[MAIN] assigned values to client struct's attributes. Now initializing mutex.\n");
        
        /* Initialize the client lock immediately */
        pthread_mutex_init(&c->lock, NULL);

        inet_ntop(AF_INET, &cliaddr.sin_addr, addrbuf, sizeof(addrbuf));
        snprintf(c->client_addr, sizeof(c->client_addr), "%s:%u", addrbuf, ntohs(cliaddr.sin_port));

        /* (Interface name lookup logic omitted for brevity, assumes same as previous) */
        log_printf("[MAIN] Creating thread.\n");
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_worker, c) != 0) {
            log_printf("[MAIN] pthread_create returned non-zero value. Closing sock and freeing.\n");
            close(csock); free(c);
        } else {
            log_printf("[MAIN] pthread_create returned 0. Detaching tid.\n");
            pthread_detach(tid);
        }
    }
    return 0;
}