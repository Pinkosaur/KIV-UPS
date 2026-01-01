/* main.c */
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

/* Global configuration limits */
int max_rooms = -1;
int max_players = -1;

int main(int argc, char *argv[]) {
    struct in_addr bind_addr;
    bind_addr.s_addr = htonl(INADDR_ANY);
    int port = DEFAULT_PORT;

    if (argc > 5) {
        fprintf(stderr, "Usage: %s [IP|any] [PORT] [MAX_ROOMS] [MAX_PLAYERS]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2 && strcmp(argv[1], "any") != 0 && strcmp(argv[1], "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, argv[1], &bind_addr) != 1) return EXIT_FAILURE;
    }
    if (argc >= 3) port = atoi(argv[2]);
    if (argc >= 4) max_rooms = atoi(argv[3]);
    if (argc >= 5) max_players = atoi(argv[4]);

    list_local_interfaces();
    
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = bind_addr;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, BACKLOG) < 0) { perror("listen"); return 1; }

    log_printf("Server listening on port %d\n", port);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int csock = accept(srv, (struct sockaddr *)&cliaddr, &clilen);
        if (csock < 0) continue;

        Client *c = calloc(1, sizeof(Client));
        if (!c) { close(csock); continue; }

        c->sock = csock;
        c->color = -1;
        c->paired = 0;
        c->match = NULL;
        c->seq = -1; 
        c->error_count = 0;
        c->state = STATE_HANDSHAKE;
        
        pthread_mutex_init(&c->lock, NULL);

        char addrbuf[ADDR_LEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, addrbuf, sizeof(addrbuf));
        snprintf(c->client_addr, sizeof(c->client_addr), "%s:%u", addrbuf, ntohs(cliaddr.sin_port));

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_worker, c) != 0) {
            close(csock); free(c);
        } else {
            pthread_detach(tid);
        }
    }
    return 0;
}