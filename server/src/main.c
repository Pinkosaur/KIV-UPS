/* main.c - start server with configurable address/port/limits */
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

int main(int argc, char *argv[]) {
    /* default settings */
    struct in_addr bind_addr;
    bind_addr.s_addr = htonl(INADDR_ANY);
    int port = DEFAULT_PORT;
    int max_rooms = -1; /* -1 = unlimited */
    int max_players = -1;

    /* Argument order:
       argv[1] = IP or "any" (optional)
       argv[2] = PORT (optional)
       argv[3] = MAX_ROOMS (optional, -1 = unlimited)
       argv[4] = MAX_PLAYERS (optional, -1 = unlimited)
    */

    if (argc > 5) {
        fprintf(stderr, "Usage: %s [IP|any] [PORT] [MAX_ROOMS] [MAX_PLAYERS]\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* parse IP if provided */
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

    /* parse port if provided */
    if (argc >= 3) {
        char *endptr = NULL;
        long p = strtol(argv[2], &endptr, 10);
        if (endptr == argv[2] || *endptr != '\0' || p < 1 || p > 65535) {
            fprintf(stderr, "Invalid port '%s' (expected 1..65535)\n", argv[2]);
            return EXIT_FAILURE;
        }
        port = (int)p;
    }

    /* parse max_rooms if provided */
    if (argc >= 4) {
        char *endptr = NULL;
        long r = strtol(argv[3], &endptr, 10);
        if (endptr == argv[3] || *endptr != '\0') {
            fprintf(stderr, "Invalid max_rooms '%s'\n", argv[3]);
            return EXIT_FAILURE;
        }
        if (r == -1) {
            max_rooms = -1;
        } else if (r >= 1) {
            max_rooms = (int)r;
        } else {
            fprintf(stderr, "Invalid max_rooms '%s' (use -1 for unlimited or >=1)\n", argv[3]);
            return EXIT_FAILURE;
        }
    }

    /* parse max_players if provided */
    if (argc >= 5) {
        char *endptr = NULL;
        long pp = strtol(argv[4], &endptr, 10);
        if (endptr == argv[4] || *endptr != '\0') {
            fprintf(stderr, "Invalid max_players '%s'\n", argv[4]);
            return EXIT_FAILURE;
        }
        if (pp == -1) {
            max_players = -1;
        } else if (pp >= 2) {
            max_players = (int)pp;
        } else {
            fprintf(stderr, "Invalid max_players '%s' (use -1 for unlimited or >=2)\n", argv[4]);
            return EXIT_FAILURE;
        }
    }

    list_local_interfaces();
    putchar('\n');

    /* Print configuration summary */
    char addrbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &bind_addr, addrbuf, sizeof(addrbuf));
    printf("Server configuration:\n");
    printf("  bind IP       : %s\n", addrbuf);
    printf("  port          : %d\n", port);
    if (max_rooms == -1) printf("  max rooms     : unlimited\n"); else printf("  max rooms     : %d\n", max_rooms);
    if (max_players == -1) printf("  max players   : unlimited\n"); else printf("  max players   : %d\n", max_players);

    int srv;
    struct sockaddr_in addr;
    int opt = 1;

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    if (setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        perror("setsockopt SO_REUSEADDR");
        /* non-fatal, continue */
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = bind_addr;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); shutdown(srv, SHUT_RDWR); close(srv); return 1;
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); shutdown(srv, SHUT_RDWR); close(srv); return 1;
    }

    printf("Server listening on %s:%d\n", addrbuf, port);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int csock = accept(srv, (struct sockaddr *)&cliaddr, &clilen);
        if (csock < 0) {
            perror("accept");
            continue;
        }

        Client *c = calloc(1, sizeof(Client));
        if (!c) {
            shutdown(csock, SHUT_RDWR);
            close(csock);
            continue;
        }

        c->sock = csock;
        c->color = -1;
        c->paired = 0;
        c->match = NULL;
        c->name[0] = '\0';
        c->client_addr[0] = '\0';
        c->server_ifname[0] = '\0';
        c->server_ip[0] = '\0';

        /* record remote (client) address:port */
        {
            char caddrbuf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cliaddr.sin_addr, caddrbuf, sizeof(caddrbuf));
            snprintf(c->client_addr, sizeof(c->client_addr), "%s:%u", caddrbuf, ntohs(cliaddr.sin_port));
        }

        /* record which local interface/address the socket is bound to (use getsockname) */
        {
            struct sockaddr_in localaddr;
            socklen_t llen = sizeof(localaddr);
            if (getsockname(csock, (struct sockaddr *)&localaddr, &llen) == 0) {
                inet_ntop(AF_INET, &localaddr.sin_addr, c->server_ip, sizeof(c->server_ip));
                /* find interface name that owns this IP, if any */
                if (!get_interface_name_for_addr(localaddr.sin_addr, c->server_ifname, sizeof(c->server_ifname))) {
                    /* fallback to empty or "unknown" */
                    strncpy(c->server_ifname, "unknown", sizeof(c->server_ifname)-1);
                    c->server_ifname[sizeof(c->server_ifname)-1] = '\0';
                }
            } else {
                strncpy(c->server_ifname, "unknown", sizeof(c->server_ifname)-1);
                c->server_ifname[sizeof(c->server_ifname)-1] = '\0';
                c->server_ip[0] = '\0';
            }
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_worker, c) != 0) {
            perror("pthread_create");
            shutdown(csock, SHUT_RDWR);
            close(csock);
            free(c);
        } else {
            pthread_detach(tid);
        }
    }

    shutdown(srv, SHUT_RDWR);
    close(srv);
    return 0;
}
