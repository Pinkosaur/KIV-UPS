/* client.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include "client.h"
#include "match.h"
#include "game.h"
#include "logging.h"

extern int max_players;
static int current_players = 0;
static pthread_mutex_t players_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Protocol Helpers --- */

void send_raw(int sock, const char *msg) {
    if (sock > 0) send(sock, msg, strlen(msg), MSG_NOSIGNAL);
}

void send_line(int sock, const char *msg) {
    if (sock <= 0) return;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s\n", msg);
    send_raw(sock, tmp);
}

void send_fmt_with_seq(Client *c, const char *fmt, ...) {
    if (!c || c->sock <= 0) return;
    char payload[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(payload, sizeof(payload), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&c->lock);
    int next = ( (c->seq + 1) & 0x1FF );
    c->seq = next;
    char out[768];
    snprintf(out, sizeof(out), "%s/%03d", payload, next);
    send_line(c->sock, out);
    pthread_mutex_unlock(&c->lock);
    
    log_printf("SENT -> %s : %s\n", c->name[0] ? c->name : "unknown", out);
}

void send_short_ack(Client *c, const char *ack_code, int recv_seq) {
    if (!c || c->sock <= 0) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/%03d", ack_code, recv_seq & 0x1FF);
    send_line(c->sock, buf);
}

void send_error(Client *c, const char *reason) {
    send_fmt_with_seq(c, "ERR %s", reason);
}

int parse_seq_number(const char *line) {
    if (!line) return -1;
    const char *p = strrchr(line, '/');
    if (!p) return -1;
    return atoi(p+1) & 0x1FF;
}

void strip_trailing_seq(char *buf) {
    char *p = strrchr(buf, '/');
    if (p) *p = '\0';
}

void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

const char *ack_code_for_received(const char *cmd) {
    if (strncmp(cmd, "HELLO", 5) == 0) return HELLO_ACK;
    if (strncmp(cmd, "MV", 2) == 0) return MV_ACK;
    if (strncmp(cmd, "RES", 3) == 0) return RES_ACK_CS;
    if (strncmp(cmd, "DRW", 3) == 0) return DRW_OFF_ACK_CS;
    return SM_ACK;
}

/* --- Client Worker --- */

void *client_worker(void *arg) {
    Client *me = (Client *)arg;
    char readbuf[BUF_SZ];
    char linebuf[LINEBUF_SZ];
    size_t lp = 0;
    ssize_t r;

    pthread_mutex_lock(&players_lock);
    current_players++;
    pthread_mutex_unlock(&players_lock);

    log_printf("New connection: %s\n", me->client_addr);

    /* Init Heartbeat */
    me->last_heartbeat = time(NULL); 

    /* 1. Handshake Phase */
    send_fmt_with_seq(me, "WELCOME ChessServer");

    int got_hello = 0;
    int is_reconnect = 0;

    while (!got_hello) {
        r = recv(me->sock, readbuf, sizeof(readbuf), 0);
        if (r <= 0) goto disconnect;

        for (ssize_t i = 0; i < r; ++i) {
            if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
            if (readbuf[i] == '\n') {
                linebuf[lp] = '\0';
                trim_crlf(linebuf);
                
                /* Handle PING during handshake if needed (unlikely but safe) */
                if (strcmp(linebuf, "PING") == 0) {
                    send_line(me->sock, "PNG");
                    lp = 0;
                    continue;
                }
                
                int seq = parse_seq_number(linebuf);
                strip_trailing_seq(linebuf);

                if (strncmp(linebuf, "HELLO ", 6) == 0) {
                    char name[64];
                    strncpy(name, linebuf + 6, sizeof(name)-1);
                    name[sizeof(name)-1] = '\0';

                    Client *old_session = match_reconnect(name, me->sock);
                    if (old_session) {
                        free(me); 
                        me = old_session;
                        send_short_ack(me, HELLO_ACK, seq);
                        send_fmt_with_seq(me, "RESUME");
                        log_printf("Resumed session for %s\n", me->name);
                        got_hello = 1;
                        is_reconnect = 1;
                        break;
                    }

                    strncpy(me->name, name, sizeof(me->name)-1);
                    if (seq >= 0) {
                        pthread_mutex_lock(&me->lock);
                        me->seq = seq;
                        pthread_mutex_unlock(&me->lock);
                        send_short_ack(me, HELLO_ACK, seq);
                    }
                    got_hello = 1;
                    log_printf("Client identified as: %s\n", me->name);
                } else {
                    send_error(me, "Invalid protocol header");
                    goto disconnect;
                }
                lp = 0;
            }
        }
    }

    if (is_reconnect) {
         goto game_loop;
    }

    while (1) {
        me->match = NULL;
        me->paired = 0;
        me->color = -1;

        send_fmt_with_seq(me, "LOBBY (CMD: LIST, NEW, JOIN <id>)");

        int in_lobby = 1;
        lp = 0;

        /* --- LOBBY LOOP --- */
        while (in_lobby) {
            r = recv(me->sock, readbuf, sizeof(readbuf), 0);
            if (r <= 0) goto disconnect;
            
            /* Update heartbeat on data */
            me->last_heartbeat = time(NULL);

            for (ssize_t i = 0; i < r; ++i) {
                if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
                if (readbuf[i] == '\n') {
                    linebuf[lp] = '\0';
                    trim_crlf(linebuf);
                    
                    /* Handle PING in Lobby */
                    if (strcmp(linebuf, "PING") == 0) {
                        send_line(me->sock, "PNG");
                        lp = 0;
                        continue;
                    }

                    int seq = parse_seq_number(linebuf);
                    strip_trailing_seq(linebuf);

                    if (seq >= 0) {
                         pthread_mutex_lock(&me->lock);
                         me->seq = seq;
                         pthread_mutex_unlock(&me->lock);
                         send_short_ack(me, SM_ACK, seq);
                    }

                    if (strcmp(linebuf, "LIST") == 0) {
                        char *l = get_room_list_str();
                        if (l) { send_fmt_with_seq(me, "ROOMLIST %s", l); free(l); }
                    } 
                    else if (strcmp(linebuf, "NEW") == 0) {
                        Match *m = match_create(me);
                        if (!m) send_error(me, "Server limit reached");
                        else {
                            me->match = m; me->color = 0;
                            send_fmt_with_seq(me, "WAITING Room %d", m->id);
                            in_lobby = 0; 
                        }
                    }
                    else if (strncmp(linebuf, "JOIN ", 5) == 0) {
                        int id = atoi(linebuf + 5);
                        Match *m = find_open_room(id);
                        if (!m) send_error(me, "Room full or closed");
                        else {
                            if (match_join(m, me) == 0) {
                                me->match = m; me->color = 1; me->paired = 1; 
                                m->white->paired = 1; in_lobby = 0;
                                notify_start(m);
                            } else send_error(me, "Join failed");
                        }
                    }
                    else {
                        send_error(me, "Unknown command");
                        goto disconnect;
                    }
                    lp = 0;
                }
            }
        }

        /* --- WAIT LOOP --- */
        if (me->color == 0 && me->match) {
            while (!me->paired) {
                char buf[256];
                ssize_t r = recv(me->sock, buf, sizeof(buf)-1, MSG_DONTWAIT);
                if (r > 0) {
                    buf[r] = '\0';
                    /* Update heartbeat */
                    me->last_heartbeat = time(NULL);
                    
                    if (strstr(buf, "EXT")) {
                        match_release_after_client(me); 
                        break;
                    }
                    /* Handle PING in Wait Loop */
                    if (strstr(buf, "PING")) {
                        send_line(me->sock, "PNG");
                    }
                } else if (r == 0) {
                    goto disconnect;
                }
                if (me->match && me->match->finished) break;
                usleep(100000); 
            }
        }

        if (!me->match || me->match->finished) {
            match_release_after_client(me);
            continue; 
        }

        /* --- GAME LOOP --- */
game_loop:
        {
            Match *myMatch = me->match;
            int game_active = 1;
            lp = 0;

            while (game_active) {
                r = recv(me->sock, readbuf, sizeof(readbuf), 0);
                if (r <= 0) { 
                    match_release_after_client(me); 
                    goto disconnect_cleanup_only_no_free; 
                }
                
                me->last_heartbeat = time(NULL);

                for (ssize_t i = 0; i < r; ++i) {
                    if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
                    if (readbuf[i] == '\n') {
                        linebuf[lp] = '\0';
                        trim_crlf(linebuf);
                        
                        /* Handle PING in Game Loop */
                        if (strcmp(linebuf, "PING") == 0) {
                            send_line(me->sock, "PNG");
                            lp = 0;
                            continue;
                        }

                        int seq = parse_seq_number(linebuf);
                        strip_trailing_seq(linebuf);

                        if (seq >= 0) {
                            pthread_mutex_lock(&me->lock);
                            me->seq = seq;
                            pthread_mutex_unlock(&me->lock);
                            send_short_ack(me, ack_code_for_received(linebuf), seq);
                        }

                        pthread_mutex_lock(&myMatch->lock);
                        
                        if (myMatch->finished) {
                            pthread_mutex_unlock(&myMatch->lock);
                            game_active = 0;
                            break;
                        }

                        if (strncmp(linebuf, "MV", 2) == 0) {
                             if (myMatch->turn != me->color) {
                                pthread_mutex_unlock(&myMatch->lock);
                                send_error(me, "Not your turn");
                            } else {
                                char *mv = linebuf + 2;
                                int r1, c1, r2, c2;
                                parse_move(mv, &r1, &c1, &r2, &c2);
                                char promo = (strlen(mv) >= 5) ? mv[4] : 0;
                                apply_move(myMatch, r1, c1, r2, c2, promo);
                                match_append_move(myMatch, mv);

                                send_fmt_with_seq(me, "OK_MV");
                                Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                                if (opp && opp->sock > 0) send_fmt_with_seq(opp, "OPP_MV %s", mv);

                                int opp_col = 1 - me->color;
                                int in_chk = is_in_check(&myMatch->state, opp_col);
                                int has_mv = has_any_legal_move(myMatch, opp_col);
                                
                                if (in_chk && !has_mv) {
                                    myMatch->finished = 1;
                                    send_fmt_with_seq(me, "WIN_CHKM");
                                    if (opp && opp->sock > 0) send_fmt_with_seq(opp, "CHKM");
                                    game_active = 0;
                                } else if (!in_chk && !has_mv) {
                                    myMatch->finished = 1;
                                    send_fmt_with_seq(me, "SM");
                                    if (opp && opp->sock > 0) send_fmt_with_seq(opp, "SM");
                                    game_active = 0;
                                } else if (in_chk) {
                                    if (opp && opp->sock > 0) send_fmt_with_seq(opp, "CHK");
                                }

                                if (!myMatch->finished) {
                                    myMatch->turn = 1 - myMatch->turn;
                                    myMatch->last_move_time = time(NULL);
                                }
                                pthread_mutex_unlock(&myMatch->lock);
                            }
                        }
                        else if (strncmp(linebuf, "RES", 3) == 0) {
                            myMatch->finished = 1;
                            send_fmt_with_seq(me, "RES");
                            Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                            if (opp && opp->sock > 0) send_fmt_with_seq(opp, "OPP_RES");
                            pthread_mutex_unlock(&myMatch->lock);
                            game_active = 0;
                        }
                        else if (strncmp(linebuf, "EXT", 3) == 0) {
                            myMatch->finished = 1;
                            Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                            if (opp) send_fmt_with_seq(opp, "OPP_EXT");
                            pthread_mutex_unlock(&myMatch->lock);
                            game_active = 0;
                        }
                        else if (strncmp(linebuf, "LIST", 4) == 0) {
                             pthread_mutex_unlock(&myMatch->lock);
                        }
                        else {
                            pthread_mutex_unlock(&myMatch->lock);
                            send_error(me, "Unknown game command");
                            goto disconnect;
                        }
                        lp = 0;
                    }
                }
                if (myMatch->finished) game_active = 0;
            }
            match_release_after_client(me);
        }
    }

disconnect:
    match_release_after_client(me);
    
    if (me->match != NULL && me->sock == -1) {
         goto disconnect_cleanup_only_no_free;
    }

    free(me);
    
disconnect_cleanup_only_no_free:
    pthread_mutex_lock(&players_lock);
    current_players--;
    pthread_mutex_unlock(&players_lock);
    log_printf("Client thread exited.\n");
    return NULL;
}