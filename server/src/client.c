#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include "client.h"
#include "match.h"
#include "game.h"


/* global waiting client (simple queue of length 1) */
static Client *waiting_client = NULL;
static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;


/* helper: send a line (no automatic newline added) */
void send_raw(int sock, const char *msg) {
    if (sock > 0) send(sock, msg, strlen(msg), 0);
}

/* helper: send line with newline */
void send_line(int sock, const char *msg) {
    if (sock <= 0) return;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s\n", msg);
    send_raw(sock, tmp);
}

/* utility: send error */
void send_error(Client *c, const char *reason) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "ERROR %s", reason);
    send_line(c->sock, tmp);
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
            send_line(other->sock, reason_to_opponent);
    }
}

void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) { s[--n] = '\0'; }
}

/* client thread */
void *client_worker(void *arg) {
    Client *me = (Client *)arg;
    char readbuf[BUF_SZ];
    char linebuf[LINEBUF_SZ];
    size_t lp = 0;
    ssize_t r;

    send_line(me->sock, "WELCOME ChessServer");

    /* ask for HELLO */
    send_line(me->sock, "SEND HELLO <name>");

    /* simple read loop until HELLO received */
    int got_hello = 0;
    while (!got_hello) {
        r = recv(me->sock, readbuf, sizeof(readbuf), 0);
        if (r <= 0) { shutdown(me->sock, SHUT_RDWR); close(me->sock); free(me); return NULL; }
        for (ssize_t i = 0; i < r; ++i) {
            if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
            if (readbuf[i] == '\n') {
                linebuf[lp] = '\0';
                trim_crlf(linebuf);
                if (strncmp(linebuf, "HELLO ", 6) == 0) {
                    strncpy(me->name, linebuf + 6, sizeof(me->name)-1);
                    me->name[sizeof(me->name)-1] = '\0';
                    got_hello = 1;
                    break;
                } else {
                    send_line(me->sock, "ERROR Expecting HELLO <name>");
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
        send_line(me->sock, "WAITING");
        printf("%s is waiting for opponent\n", me->name);

        /* wait until paired but also detect if client closed connection.
           Server now enforces matchmaking timeout (MATCHMAKING_TIMEOUT_SECONDS) server-side.
        */
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

                /* server-side message (explicit): MATCHMAKING_TIMEOUT */
                send_line(me->sock, "MATCHMAKING_TIMEOUT");

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
            send_line(me->sock, "ERROR Server internal");
            shutdown(me->sock, SHUT_RDWR);
            close(me->sock);
            free(me);
            return NULL;
        }
        /* assign */
        op->match = m; op->paired = 1; op->color = 0; /* white */
        me->match = m; me->paired = 1; me->color = 1; /* black */

        /* notify start */
        notify_start(m);
        printf("Paired %s (white, peer %s) <-> %s (black, peer %s)  [on %s/%s]\n",
               op->name, op->client_addr, me->name, me->client_addr, op->server_ifname, op->server_ip);
    }

    /* At this point me->match should be set by pairing routine */
    /* If we were the first waiting, other thread created the match and set me->match;
       if we were second, we already set match above. */

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
                /* process command */
                if (strncmp(linebuf, "MOVE ", 5) == 0) {
                    const char *mv = linebuf + 5;
                    if (!is_move_format(mv)) {
                        send_error(me, "Bad MOVE format");
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

                                    /* send OK to mover first (client expects OK_MOVE) */
                                    send_line(me->sock, "OK_MOVE");

                                    /* always notify opponent of the move (so both boards update) */
                                    if (opp && opp->sock > 0) {
                                        char buf[128];
                                        snprintf(buf, sizeof(buf), "OPPONENT_MOVE %s", mv);
                                        send_line(opp->sock, buf);
                                    }

                                    /* if opponent is in check but not mate -> send CHECK to opponent */
                                    if (opp_in_check && opp_has_move) {
                                        if (opp && opp->sock > 0) send_line(opp->sock, "CHECK");
                                    }

                                    /* if it's checkmate -> send pair of messages (winner/loser) */
                                    if (opp_in_check && !opp_has_move) {
                                        /* mover delivered checkmate to opponent */
                                        if (opp && opp->sock > 0) send_line(opp->sock, "CHECKMATE");
                                        send_line(me->sock, "CHECKMATE_WIN");
                                        printf("%s won by checkmate against %s\n", me->name, opp->name);
                                    }

                                    /* stalemate: no legal moves and not in check */
                                    if (!opp_in_check && !opp_has_move) {
                                        /* send STALEMATE to both sides so they can display neutral overlay */
                                        if (opp && opp->sock > 0) send_line(opp->sock, "STALEMATE");
                                        send_line(me->sock, "STALEMATE");
                                        printf("%s and %s ended up in a stalemate\n", me->name, opp->name);
                                    }

                                    /* flip turn only if game continues */
                                    if (!myMatch->finished) {
                                        myMatch->turn = 1 - myMatch->turn;
                                        myMatch->last_move_time = time(NULL); /* start timer for new player */
                                        pthread_mutex_unlock(&myMatch->lock);
                                    } else {
                                        /* send BYE to both clients (if still connected) and close their sockets server-side */
                                        if (myMatch->white && myMatch->white->sock > 0) {
                                            send_line(myMatch->white->sock, "BYE");
                                            shutdown(myMatch->white->sock, SHUT_RDWR); /* wake any blocked recv on that socket */
                                            close(myMatch->white->sock);
                                            myMatch->white->sock = -1;
                                        }
                                        if (myMatch->black && myMatch->black->sock > 0) {
                                            send_line(myMatch->black->sock, "BYE");
                                            shutdown(myMatch->black->sock, SHUT_RDWR);
                                            close(myMatch->black->sock);
                                            myMatch->black->sock = -1;
                                        }
                                        pthread_mutex_unlock(&myMatch->lock);
                                        goto cleanup;
                                    }
                                }
                            }
                        }
                    }
                } else if (strncmp(linebuf, "RESIGN", 6) == 0) {
                    /* mark finished and notify opponent */
                    pthread_mutex_lock(&myMatch->lock);
                    myMatch->finished = 1;
                    myMatch->last_move_time = 0;
                    pthread_mutex_unlock(&myMatch->lock);

                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    
                    /* Send RESIGN to resigning player (they lost) */
                    send_line(me->sock, "RESIGN");
                    printf("%s resigned against %s\n", me->name, opp->name);
                    /* Send OPPONENT_RESIGNED to opponent (they won) */
                    if (opp && opp->sock > 0) {
                        send_line(opp->sock, "OPPONENT_RESIGNED");
                    }
                    goto cleanup;
                } else if (strncmp(linebuf, "DRAW_OFFER", 10) == 0) {
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
                        send_line(opp->sock, "DRAW_OFFER");
                    } else {
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "No opponent");
                    }
                } else if (strncmp(linebuf, "DRAW_ACCEPT", 11) == 0) {
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

                        /* Send DRAW_ACCEPTED to both if connected (use the copied fds) */
                        if (opp_sock > 0) send_line(opp_sock, "DRAW_ACCEPTED");
                        if (me_sock  > 0) send_line(me_sock,  "DRAW_ACCEPTED");

                        /* Important: wake blocked recv() callers on both sockets, then send BYE and close.
                        shutdown() ensures the peer thread's recv() returns promptly so it can run its cleanup. */
                        if (opp_sock > 0) {
                            /* send BYE, then shutdown and close */
                            send_line(opp_sock, "BYE");
                            shutdown(opp_sock, SHUT_RDWR);
                            close(opp_sock);
                        }
                        if (me_sock > 0) {
                            send_line(me_sock, "BYE");
                            shutdown(me_sock, SHUT_RDWR);
                            close(me_sock);
                        }

                        /* now jump to cleanup; the usual cleanup code (or match_release_after_client) will handle match freeing */
                        goto cleanup;
                    }
                } else if (strncmp(linebuf, "DRAW_DECLINE", 12) == 0) {
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
                            send_line(opp->sock, "DRAW_DECLINED");
                        }
                    }              
                } else if (strncmp(linebuf, "QUIT", 4) == 0) {
                    /* Treat explicit QUIT as opponent leaving -> opponent wins */
                    pthread_mutex_lock(&myMatch->lock);
                    myMatch->finished = 1;
                    myMatch->last_move_time = 0;
                    pthread_mutex_unlock(&myMatch->lock);

                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s quit their match against %s\n", me->name, opp->name);
                    if (opp && opp->sock > 0) {
                        /* Notify opponent that their opponent quit and that they won */
                        send_line(opp->sock, "OPPONENT_QUIT");
                    }
                    /* send BYE to quitting client and cleanup their thread */
                    send_line(me->sock, "BYE");
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