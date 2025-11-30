#ifndef CLIENT_H
#define CLIENT_H

#include <net/if.h>
#include <netinet/in.h>
#include "match.h"

#define BUF_SZ 1024
#define LINEBUF_SZ 4096
#define MATCHMAKING_TIMEOUT_SECONDS 10

/* Client struct */
typedef struct Client {
    int sock;
    char name[64];
    int color;
    int paired;
    Match *match;
    char client_addr[64];
    char server_ifname[IF_NAMESIZE];
    char server_ip[INET_ADDRSTRLEN];
} Client;

/* client thread entry */
void *client_worker(void *arg);

/* send helpers */
void send_raw(int sock, const char *msg);
void send_line(int sock, const char *msg);
void send_error(Client *c, const char *reason);
void send_line_client(Client *c, const char *msg);

/* utility used by server & client */
void match_close_and_notify(Match *m, Client *leaver, const char *reason_to_opponent);

void trim_crlf(char *s);

#endif /* CLIENT_H */
