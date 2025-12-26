/* Modified client.c
 *
 * Key behavior:
 *  - Incoming lines may end with "/NNN" where NNN is a 3-digit seq (0..511).
 *  - After parsing an incoming message, server immediately replies with a short
 *    acknowledgement in the form "XX/YYY" (two-digit ack code / three-digit seq),
 *    where XX is the ack-code corresponding to the received message type and
 *    YYY is the received seq (echoed).
 *  - When the server sends any application message to the client, it appends
 *    "/%03d" where the seq is computed as (me->seq + 1) % 512 and then updates
 *    me->seq to that value. This keeps a single 'seq' field per Client that is
 *    used to derive the next seq.
 *
 *  - The server never blocks waiting for ACKs. (Client must implement wait/ retry.)
 *
 * Assumptions:
 *  - client.h defines the various ACK macros, e.g. HELLO_ACK, MV_ACK, etc.
 *  - Client struct contains at least: int sock; char name[]; char client_addr[]; int seq; ...
 *
 * Compile-time: include this file in your project as replacement for previous client.c
 */

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

/* global waiting client (simple queue of length 1) */
static Client *waiting_client = NULL;
static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;

/* helper: send a raw buffer (no newline) */
void send_raw(int sock, const char *msg) {
    if (sock > 0) send(sock, msg, strlen(msg), MSG_NOSIGNAL);
}

/* helper: send line with newline */
void send_line(int sock, const char *msg) {
    if (sock <= 0) return;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s\n", msg);
    send_raw(sock, tmp);
}

/* helper: format a message, append /%03d seq (computed from c->seq) and send it */
void send_fmt_with_seq(Client *c, const char *fmt, ...) {
    if (!c || c->sock <= 0) return;
    char payload[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(payload, sizeof(payload), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&c->lock);
    /* compute next seq = (last_seq + 1) % 512 */
    int next = ( (c->seq + 1) & 0x1FF ); /* modulo 512 */
    char out[768];
    snprintf(out, sizeof(out), "%s/%03d", payload, next);
    /* update stored seq to reflect last sent/processed seq (single seq derivation) */
    c->seq = next;
    send_line(c->sock, out);
    pthread_mutex_unlock(&c->lock);
    printf("SENT -> %s (%s) : %s\n", c->name[0] ? c->name : "(unknown)", c->client_addr, out);
}

/* utility: send short ack code (two-digit) echoing a received seq (three-digit) */
void send_short_ack(Client *c, const char *ack_code, int recv_seq) {
    if (!c || c->sock <= 0) return;
    char buf[64];
    /* ack format: "XX/YYY" */
    snprintf(buf, sizeof(buf), "%s/%03d", ack_code, recv_seq & 0x1FF);
    send_line(c->sock, buf);
    printf("SENT-ACK -> %s (%s) : %s\n", c->name[0] ? c->name : "(unknown)", c->client_addr, buf);
}

/* utility: send error (long form). This will attach a seq as well. */
void send_error(Client *c, const char *reason) {
    /* send formatted error with seq */
    send_fmt_with_seq(c, "ERR %s", reason);
}

/* Send a textual line to a Client* and log to server console.
   Keeps a single place for logging so every message the client receives is visible on server. */
void send_line_client(Client *c, const char *msg) {
    if (!c) return;
    if (c->sock > 0) {
        send_line(c->sock, msg);
        printf("SENT -> %s (%s) : %s\n", c->name[0] ? c->name : "(unknown)", c->client_addr, msg);
    } else {
        /* No socket available (already closed) — still log the intent. */
        printf("SENT -> %s (no socket) : %s\n", c->name[0] ? c->name : "(unknown)", msg);
    }
}

/* close a match and notify the other side */
void match_close_and_notify(Match *m, Client *leaver, const char *reason_to_opponent) {
    if (!m) return;
    Client *other = (leaver == m->white) ? m->black : m->white;
    if (other && other->sock > 0) {
        if (reason_to_opponent)
            /* use seq-annotated send so the client can ack server messages */
            send_fmt_with_seq(other, "%s", reason_to_opponent);
    }
}

void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) { s[--n] = '\0'; }
}

void close_sockets(Match *myMatch) {
    if (myMatch->white && myMatch->white->sock > 0) {
        shutdown(myMatch->white->sock, SHUT_RDWR); /* wake any blocked recv on that socket */
        close(myMatch->white->sock);
        myMatch->white->sock = -1;
    }
    if (myMatch->black && myMatch->black->sock > 0) {
        shutdown(myMatch->black->sock, SHUT_RDWR);
        close(myMatch->black->sock);
        myMatch->black->sock = -1;
    }
}

/* parse trailing "/NNN" sequence number from a message.
   Returns -1 if not present or malformed; otherwise 0..511.
   Does not modify the input string. */
int parse_seq_number(const char *line) {
    if (!line) return -1;
    const char *p = strrchr(line, '/');
    if (!p) return -1;
    /* expect three digits at least (but accept 1..3 digits) */
    p++;
    int len = strlen(p);
    if (len < 1 || len > 4) {
        /* if there is a newline it should already be trimmed, keep conservative */
        return -1;
    }
    char digits[8];
    strncpy(digits, p, sizeof(digits)-1);
    digits[sizeof(digits)-1] = '\0';
    /* reject non-digits */
    for (int i = 0; digits[i]; ++i) if (digits[i] < '0' || digits[i] > '9') return -1;
    int v = atoi(digits);
    if (v < 0 || v > 511) return -1;
    return v;
}

/* strip trailing "/NNN" part in-place (so the caller can reuse the same buffer)
   Returns 1 if stripped, 0 if no trailing seq found. */
int strip_trailing_seq(char *buf) {
    char *p = strrchr(buf, '/');
    if (!p) return 0;
    /* Ensure the part after / is digits only */
    char *q = p + 1;
    if (*q == '\0') return 0;
    for (; *q; ++q) if (*q < '0' || *q > '9') return 0;
    /* valid numeric suffix -> terminate string at '/' */
    *p = '\0';
    return 1;
}

/* decide ack code for a received client message (two-digit string).
   Return pointer to a static string (do not free). */
const char *ack_code_for_received(const char *line_no_seq) {
    if (strncmp(line_no_seq, "HELLO ", 6) == 0) return HELLO_ACK; /* 18 */
    if (strncmp(line_no_seq, "MV", 2) == 0) return MV_ACK;       /* 19 */
    if (strncmp(line_no_seq, "DRW_OFF", 7) == 0) return DRW_OFF_ACK_CS; /* 20 */
    if (strncmp(line_no_seq, "DRW_DEC", 7) == 0) return DRW_DEC_ACK; /* 21 */
    if (strncmp(line_no_seq, "DRW_ACC", 7) == 0) return DRW_ACC_ACK; /* 22 */
    if (strncmp(line_no_seq, "RES", 3) == 0) return RES_ACK_CS; /* 23 */
    if (strncmp(line_no_seq, "EXT", 3) == 0) return OPP_EXT_ACK; /* 17 used as generic? (server may define) */
    /* fallback to generic small message ack (SM=25) */
    return SM_ACK;
}

/* client thread */
void *client_worker(void *arg) {
    Client *me = (Client *)arg;
    char readbuf[BUF_SZ];
    char linebuf[LINEBUF_SZ];
    size_t lp = 0;
    ssize_t r;
    pthread_mutex_init(&me->lock, NULL);

    /* initialize seq to -1 meaning "no seq seen yet" -> next seq derived from first inbound message */
    if (me->seq < 0) me->seq = -1;

    /* send initial welcome + instruction (server->client message) using seq-annotated send.
       Use send_fmt_with_seq so clients will receive a message ending with "/NNN". */
    send_fmt_with_seq(me, "WELCOME ChessServer");
    send_fmt_with_seq(me, "SEND HELLO <name>");

    /* simple read loop until HELLO received from client; handle possible trailing /NNN seq */
    int got_hello = 0;
    lp = 0;
    while (!got_hello) {
        r = recv(me->sock, readbuf, sizeof(readbuf), 0);
        if (r <= 0) { shutdown(me->sock, SHUT_RDWR); close(me->sock); free(me); return NULL; }
        for (ssize_t i = 0; i < r; ++i) {
            if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
            if (readbuf[i] == '\n') {
                linebuf[lp] = '\0';
                trim_crlf(linebuf);

                /* parse seq if present */
                int recv_seq = parse_seq_number(linebuf);
                char tmpbuf[LINEBUF_SZ];
                strncpy(tmpbuf, linebuf, sizeof(tmpbuf)-1);
                tmpbuf[sizeof(tmpbuf)-1] = '\0';
                if (recv_seq >= 0) {
                    /* strip seq from working copy */
                    strip_trailing_seq(tmpbuf);
                }

                if (strncmp(tmpbuf, "HELLO ", 6) == 0) {
                    /* extract name (without the seq) */
                    char *namepart = tmpbuf + 6;
                    strncpy(me->name, namepart, sizeof(me->name)-1);
                    me->name[sizeof(me->name)-1] = '\0';

                    /* if we got a valid seq, adopt it as last-seen seq (so next send uses seq+1) */
                    if (recv_seq >= 0) {
                        me->seq = recv_seq; /* adopt client seq (last seen) */
                    }

                    /* send immediate short ack for HELLO (echo seq) */
                    if (recv_seq >= 0) send_short_ack(me, HELLO_ACK, recv_seq);

                    got_hello = 1;
                    lp = 0;
                    break;
                } else {
                    /* client sent something unexpected before HELLO: send short ERR ack if seq present */
                    int seq_for_err = (recv_seq >= 0) ? recv_seq : ( (me->seq + 1) & 0x1FF );
                    send_short_ack(me, ERR_ACK, seq_for_err);
                    /* also send long-form error (with our seq) */
                    send_error(me, "Expecting HELLO <name>");
                }
                lp = 0;
            }
        }
    }

    printf("Client connected: %s (peer %s) accepted on %s %s (sock %d)\n",
           me->name,
           me->client_addr,
           me->server_ifname,
           (me->server_ip[0] ? me->server_ip : ""),
           me->sock);

    /* Try to pair */
    pthread_mutex_lock(&wait_mutex);
    if (waiting_client == NULL) {
        /* become waiting */
        waiting_client = me;
        pthread_mutex_unlock(&wait_mutex);
        me->color = 0; /* will be WHITE */
        me->paired = 0;

        /* send WAIT with seq */
        send_fmt_with_seq(me, "WAIT");
        printf("%s is waiting for opponent\n", me->name);

        /* wait until paired but also detect if client closed connection.
           Server enforces matchmaking timeout (MATCHMAKING_TIMEOUT_SECONDS) server-side. */
        time_t wait_start = time(NULL);
        int search_timeout = MATCHMAKING_TIMEOUT_SECONDS;
        if (search_timeout <= 0) search_timeout = 1;

        while (!me->paired) {
            usleep(200000); /* 200ms */

            /* Detect peer closure quickly with non-blocking peek */
            char tmp;
            ssize_t rr = recv(me->sock, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
            if (rr == 0) {
                /* peer closed connection — remove from waiting queue and exit */
                pthread_mutex_lock(&wait_mutex);
                if (waiting_client == me) waiting_client = NULL;
                pthread_mutex_unlock(&wait_mutex);

                shutdown(me->sock, SHUT_RDWR);
                close(me->sock);
                printf("%s disconnected while waiting (socket closed)\n", me->name);
                free(me);
                return NULL;
            } else if (rr < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    /* socket error: treat as closed */
                    pthread_mutex_lock(&wait_mutex);
                    if (waiting_client == me) waiting_client = NULL;
                    pthread_mutex_unlock(&wait_mutex);

                    shutdown(me->sock, SHUT_RDWR);
                    close(me->sock);
                    perror("recv(MSG_PEEK)");
                    printf("%s disconnected while waiting (socket error)\n", me->name);
                    free(me);
                    return NULL;
                }
                /* rr < 0 with EAGAIN/EWOULDBLOCK -> no data pending, still alive */
            }

            /* Check server-side matchmaking timeout */
            time_t now = time(NULL);
            if (difftime(now, wait_start) >= search_timeout) {
                /* timed out waiting for opponent: inform client and close connection */
                pthread_mutex_lock(&wait_mutex);
                if (waiting_client == me) waiting_client = NULL;
                pthread_mutex_unlock(&wait_mutex);

                /* server-side short MM_TOUT (match timeout) with seq */
                send_fmt_with_seq(me, "MM_TOUT");

                /* close the socket and free the client (server cleans up) */
                shutdown(me->sock, SHUT_RDWR);
                close(me->sock);
                printf("%s matchmaking timed out after %d seconds\n", me->name, search_timeout);
                free(me);
                return NULL;
            }
        }
    } else {
        /* pair with waiting client */
        Client *op = waiting_client;
        waiting_client = NULL;
        pthread_mutex_unlock(&wait_mutex);

        /* create match */
        Match *m = match_create(op, me);
        if (!m) {
            send_error(me, "Server internal");
            shutdown(me->sock, SHUT_RDWR);
            close(me->sock);
            free(me);
            return NULL;
        }
        /* assign */
        op->match = m; op->paired = 1; op->color = 0; /* white */
        me->match = m; me->paired = 1; me->color = 1; /* black */

        /* notify start - existing notify_start in match.c uses send_line(); if you want seq on those messages,
           update notify_start to use send_fmt_with_seq (or call send_fmt_with_seq here instead). */
        notify_start(m); /* you may want to modify notify_start to call send_fmt_with_seq instead of send_line */
        printf("Paired %s (white, peer %s) <-> %s (black, peer %s)  [on %s/%s]\n",
               op->name, op->client_addr, me->name, me->client_addr, op->server_ifname, op->server_ip);
    }

    /* Wait until match pointer is available (defensive) */
    while (me->match == NULL) {
        usleep(100000);
    }

    Match *myMatch = me->match;

    /* Main communication loop: read lines and act */
    lp = 0;
    while (1) {
        r = recv(me->sock, readbuf, sizeof(readbuf), 0);
        if (r < 0) {
            perror("recv");
            break;
        } else if (r == 0) {
            /* connection closed */
            printf("%s closed connection\n", me->name);
            break;
        }
        for (ssize_t i = 0; i < r; ++i) {
            if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
            if (readbuf[i] == '\n') {
                linebuf[lp] = '\0';
                trim_crlf(linebuf);

                /* parse seq if present and strip it for processing */
                int recv_seq = parse_seq_number(linebuf);
                char procbuf[LINEBUF_SZ];
                strncpy(procbuf, linebuf, sizeof(procbuf)-1);
                procbuf[sizeof(procbuf)-1] = '\0';
                if (recv_seq >= 0) strip_trailing_seq(procbuf);

                /* If a seq was present, adopt it as last-seen seq so next send uses seq+1.
                   This assumes the client and server increment the shared seq on each message.
                   That matches "derive expected seq from previous one" requirement. */
                if (recv_seq >= 0) {
                    me->seq = recv_seq;
                    /* Immediately send a short ack echoing the received seq and the ack-code
                       for this message type (abbreviated "XX/YYY") */
                    const char *ackc = ack_code_for_received(procbuf);
                    send_short_ack(me, ackc, recv_seq);
                }

                /* process command */
                if (strncmp(procbuf, "MV", 2) == 0) {
                    const char *mv = procbuf + 2;
                    if (!is_move_format(mv)) {
                        send_error(me, "Bad MV format");
                        send_error(me, procbuf);
                    } else {
                        pthread_mutex_lock(&myMatch->lock);
                        int my_color = me->color;
                        if (myMatch->finished) {
                            pthread_mutex_unlock(&myMatch->lock);
                            send_error(me, "Game over");
                        } else if (myMatch->turn != my_color) {
                            pthread_mutex_unlock(&myMatch->lock);
                            send_error(me, "Not your turn");
                        } else {
                            /* parse and validate ... */
                            int r1,c1,r2,c2;
                            parse_move(mv,&r1,&c1,&r2,&c2);

                            /* detect optional promotion char */
                            char promo = 0;
                            if (strlen(mv) == 5) promo = mv[4];

                            /* basic movement legality (ignores checks) */
                            if (!is_legal_move_basic(myMatch, my_color, r1, c1, r2, c2)) {
                                pthread_mutex_unlock(&myMatch->lock);
                                send_error(me, "Illegal move");
                            } else {
                                /* do not allow move that leaves mover's king in check */
                                if (move_leaves_in_check(myMatch, my_color, r1, c1, r2, c2)) {
                                    pthread_mutex_unlock(&myMatch->lock);
                                    send_error(me, "Move leaves king in check");
                                } else {
                                    int opp_color = 1 - my_color;

                                    /* apply move (handles ep, castling, promotion), append, then evaluate check/mate/stalemate */
                                    apply_move(myMatch, r1, c1, r2, c2, promo);
                                    match_append_move(myMatch, mv);

                                    /* determine opponent and colors */
                                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                                    opp_color = 1 - my_color;

                                    /* check whether opponent is in check now and whether they have any legal move */
                                    int opp_in_check = is_in_check(&myMatch->state, opp_color);
                                    int opp_has_move = has_any_legal_move(myMatch, opp_color);

                                    /* mark finished if mate or stalemate so future moves are rejected */
                                    if ((opp_in_check && !opp_has_move) || (!opp_in_check && !opp_has_move)) {
                                        myMatch->finished = 1;
                                        myMatch->last_move_time = 0;
                                    }

                                    /* send OK to mover first (client expects OK_MV) - with seq */
                                    send_fmt_with_seq(me, "OK_MV");

                                    /* always notify opponent of the move (so both boards update) */
                                    if (opp && opp->sock > 0) {
                                        /* send OPP_MV with mv and seq */
                                        send_fmt_with_seq(opp, "OPP_MV %s", mv);
                                    }

                                    /* if opponent is in check but not mate -> send CHK to opponent */
                                    if (opp_in_check && opp_has_move) {
                                        if (opp && opp->sock > 0) send_fmt_with_seq(opp, "CHK");
                                    }

                                    /* if it's checkmate -> send pair of messages (winner/loser) */
                                    if (opp_in_check && !opp_has_move) {
                                        /* mover delivered checkmate to opponent */
                                        if (opp && opp->sock > 0) send_fmt_with_seq(opp, "CHKM");
                                        send_fmt_with_seq(me, "WIN_CHKM");
                                        printf("%s won by checkmate against %s\n", me->name, opp->name);
                                    }

                                    /* stalemate: no legal moves and not in check */
                                    if (!opp_in_check && !opp_has_move) {
                                        /* send SM to both sides so they can display neutral overlay */
                                        if (opp && opp->sock > 0) send_fmt_with_seq(opp, "SM");
                                        send_fmt_with_seq(me, "SM");
                                        printf("%s and %s ended up in a stalemate\n", me->name, opp->name);
                                    }

                                    /* flip turn only if game continues */
                                    if (!myMatch->finished) {
                                        myMatch->turn = 1 - myMatch->turn;
                                        myMatch->last_move_time = time(NULL); /* start timer for new player */
                                        pthread_mutex_unlock(&myMatch->lock);
                                    } else {
                                        close_sockets(myMatch);
                                        pthread_mutex_unlock(&myMatch->lock);
                                        goto cleanup;
                                    }
                                }
                            }
                        }
                    }
                } else if (strncmp(procbuf, "RES", 3) == 0) {
                    /* mark finished and notify opponent */
                    pthread_mutex_lock(&myMatch->lock);
                    myMatch->finished = 1;
                    myMatch->last_move_time = 0;
                    pthread_mutex_unlock(&myMatch->lock);

                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;

                    /* Send RES to resigning player (they lost) */
                    send_fmt_with_seq(me, "RES");
                    printf("%s resigned against %s\n", me->name, opp->name);
                    /* Send OPP_RES to opponent (they won) */
                    if (opp && opp->sock > 0) {
                        send_fmt_with_seq(opp, "OPP_RES");
                    }
                    close_sockets(myMatch);
                    goto cleanup;
                } else if (strncmp(procbuf, "DRW_OFF", 7) == 0) {
                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s is offering %s a draw\n", me->name, opp->name);
                    pthread_mutex_lock(&myMatch->lock);
                    if (myMatch->draw_offered_by != -1) {
                        /* there is already a pending draw offer (reject) */
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "Draw offer already pending");
                    } else if (opp && opp->sock > 0) {
                        /* record who offered and forward offer */
                        myMatch->draw_offered_by = me->color;
                        pthread_mutex_unlock(&myMatch->lock);
                        send_fmt_with_seq(opp, "DRW_OFF");
                    } else {
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "No opponent");
                    }
                } else if (strncmp(procbuf, "DRW_ACC", 7) == 0) {
                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s accepted %s's draw offer\n", me->name, opp ? opp->name : "(unknown)");
                    pthread_mutex_lock(&myMatch->lock);
                    /* only accept a draw if opponent actually offered it */
                    if (myMatch->draw_offered_by == -1 || myMatch->draw_offered_by == me->color) {
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "No draw offer to accept");
                    } else {
                        /* mark finished and clear pending offer */
                        myMatch->draw_offered_by = -1;
                        myMatch->finished = 1;
                        myMatch->last_move_time = 0;

                        /* copy sockets locally and mark them -1 under lock so other code won't try to use them */
                        int me_sock  = me->sock;
                        int opp_sock = (opp ? opp->sock : -1);

                        /* mark sockets invalid in client structs so other code won't send on them */
                        if (opp) opp->sock = -1;
                        me->sock = -1;

                        pthread_mutex_unlock(&myMatch->lock);

                        /* Send DRW_ACD to both if connected (use the copied fds) */
                        if (opp_sock > 0) send_line(opp_sock, "DRW_ACD");
                        if (me_sock  > 0) send_line(me_sock,  "DRW_ACD");

                        usleep(2000000);
                        /* Important: wake blocked recv() callers on both sockets, then close.
                           shutdown() ensures the peer thread's recv() returns promptly so it can run its cleanup. */
                        if (opp_sock > 0) {
                            shutdown(opp_sock, SHUT_RDWR);
                            close(opp_sock);
                        }
                        if (me_sock > 0) {
                            shutdown(me_sock, SHUT_RDWR);
                            close(me_sock);
                        }

                        /* now jump to cleanup; the usual cleanup code (or match_release_after_client) will handle match freeing */
                        goto cleanup;
                    }
                } else if (strncmp(procbuf, "DRW_DEC", 7) == 0) {
                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s declined %s's draw offer\n", me->name, opp->name);
                    pthread_mutex_lock(&myMatch->lock);
                    /* decline only allowed if opponent offered */
                    if (myMatch->draw_offered_by == -1 || myMatch->draw_offered_by == me->color) {
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "No draw offer to decline");
                    } else {
                        /* inform original offerer that their offer was declined */
                        myMatch->draw_offered_by = -1;
                        pthread_mutex_unlock(&myMatch->lock);
                        if (opp && opp->sock > 0) {
                            send_fmt_with_seq(opp, "DRW_DCD");
                        }
                    }
                } else if (strncmp(procbuf, "EXT", 3) == 0) {
                    /* Treat explicit EXT as opponent leaving -> opponent wins */
                    pthread_mutex_lock(&myMatch->lock);
                    myMatch->finished = 1;
                    myMatch->last_move_time = 0;
                    pthread_mutex_unlock(&myMatch->lock);

                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s quit their match against %s\n", me->name, opp->name);
                    if (opp && opp->sock > 0) {
                        /* Notify opponent that their opponent quit and that they won */
                        send_fmt_with_seq(opp, "OPP_EXT");
                    }
                    goto cleanup;
                } else {
                    send_error(me, "Unknown command");
                }

                lp = 0;
            }
        }
    }

cleanup:
    /* cleanup client: close socket, free, notify other and free match */
    if (me->sock > 0) {
        shutdown(me->sock, SHUT_RDWR);
        close(me->sock);
    }
    printf("%s disconnected (peer %s) on %s %s\n", me->name, me->client_addr, me->server_ifname, me->server_ip);

    /* if we were in waiting queue, remove */
    pthread_mutex_lock(&wait_mutex);
    if (waiting_client == me) waiting_client = NULL;
    pthread_mutex_unlock(&wait_mutex);

    match_release_after_client(me);

    free(me);
    return NULL;
}
