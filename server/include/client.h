#ifndef CLIENT_H
#define CLIENT_H

#include <net/if.h>
#include <netinet/in.h>
#include "match.h"

#define BUF_SZ 1024
#define LINEBUF_SZ 64

/* Acknowledgement messages */
#define MM_TOUT_ACK "01"
#define WAIT_ACK "02"
#define START_ACK "03"
#define ERR_ACK "04"
#define OK_MV_ACK "05"
#define OPP_MV_ACK "06"
#define CHK_ACK "07"
#define CHKM_ACK "08"
#define WIN_CHKM_ACK "09"
#define DRW_OFF_ACK_SC "10"
#define DRW_DCD_ACK "11"
#define DRW_ACD_ACK "12"
#define RES_ACK_SC "13"
#define OPP_RES_ACK "14"
#define TOUT_ACK "15"
#define OPP_TOUT_ACK "16"
#define OPP_EXT_ACK "17"
#define SM_ACK "25"

#define HELLO_ACK "18"
#define MV_ACK "19"
#define DRW_OFF_ACK_CS "20"
#define DRW_DEC_ACK "21"
#define DRW_ACC_ACK "22"
#define RES_ACK_CS "23"
#define RESUME_ACK "26"


/* Client struct */
typedef struct Client {
    int sock;
    char name[64];
    int color; /* 0 - white, 1 - black*/
    int paired;
    Match *match;
    char client_addr[64];
    char server_ifname[IF_NAMESIZE];
    char server_ip[INET_ADDRSTRLEN];
    int seq;
    pthread_mutex_t lock;
    time_t disconnect_time;
    time_t last_heartbeat;
} Client;

/* client thread entry */
void *client_worker(void *arg);

/* send helpers */
void send_raw(int sock, const char *msg);
void send_line(int sock, const char *msg);
void send_error(Client *c, const char *reason);
void send_line_client(Client *c, const char *msg);

int wait_for_ack(Client *c, const char *ack_code);
int parse_seq_number(const char *msg);
void send_fmt_with_seq(Client *c, const char *fmt, ...);

/* utility used by server & client */
void match_close_and_notify(Match *m, Client *leaver, const char *reason_to_opponent);
void close_sockets(Match *m);

void trim_crlf(char *s);

#endif /* CLIENT_H */
