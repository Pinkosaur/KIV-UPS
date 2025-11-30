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

#define PORT 10001
#define BACKLOG 10

int main(void) {
    int srv;
    struct sockaddr_in addr;
    int opt = 1;

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    /* bind to all interfaces (0.0.0.0) so clients on the LAN can connect */
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    list_local_interfaces();

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); shutdown(srv, SHUT_RDWR); close(srv); return 1;
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); shutdown(srv, SHUT_RDWR); close(srv); return 1;
    }

    printf("Server listening on all interfaces, port %d\n", PORT);

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
            char addrbuf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cliaddr.sin_addr, addrbuf, sizeof(addrbuf));
            snprintf(c->client_addr, sizeof(c->client_addr), "%s:%u", addrbuf, ntohs(cliaddr.sin_port));
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
                }
            } else {
                strncpy(c->server_ifname, "unknown", sizeof(c->server_ifname)-1);
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
