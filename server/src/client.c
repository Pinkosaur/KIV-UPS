/* src/client.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include "client.h"
#include "match.h"
#include "game.h"
#include "logging.h"
#include "config.h"

extern int max_players;
static int current_players = 0;
static pthread_mutex_t players_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Protocol Helpers --- */

void send_raw(int sock, const char *msg) {
    if (sock > 0) send(sock, msg, strlen(msg), MSG_NOSIGNAL);
}

void send_line(int sock, const char *msg) {
    if (sock <= 0) return;
    char tmp[BUFFER_SZ];
    snprintf(tmp, sizeof(tmp), "%s\n", msg);
    send_raw(sock, tmp);
}

void send_fmt_with_seq(Client *c, const char *fmt, ...) {
    if (!c || c->sock <= 0) return;
    char payload[BIG_BUFFER_SZ];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(payload, sizeof(payload), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&c->lock);
    int next = ( (c->seq + 1) & 0x1FF );
    c->seq = next;
    char out[BIG_BUFFER_SZ + 64];
    snprintf(out, sizeof(out), "%s/%03d\n", payload, next);
    send_raw(c->sock, out);
    pthread_mutex_unlock(&c->lock);
    
    size_t len = strlen(out);
    if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';
    if (len > 128) log_printf("SENT -> %s : %.128s... (truncated)\n", c->name[0]?c->name:"unknown", out);
    else log_printf("SENT -> %s : %s\n", c->name[0]?c->name:"unknown", out);
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

int handle_protocol_error(Client *me, const char *msg) {
    me->error_count++;
    log_printf("[CLIENT %s] Protocol/Logic Error %d/%d: %s\n", me->name, me->error_count, MAX_ERRORS, msg);
    
    if (me->error_count >= MAX_ERRORS) {
        send_error(me, "Too many invalid messages. Disconnecting.");
        
        /* [FIX] Notify opponent and MARK MATCH FINISHED so it doesn't persist */
        if (me->match) {
            pthread_mutex_lock(&me->match->lock);
            if (!me->match->finished) {
                Client *opp = (me->match->white == me) ? me->match->black : me->match->white;
                if (opp && opp->sock > 0) {
                    send_fmt_with_seq(opp, "OPP_KICK");
                }
                me->match->finished = 1; /* Forces room destruction in release_after_client */
            }
            pthread_mutex_unlock(&me->match->lock);
        }
        
        return 1; /* Disconnect */
    }
    send_error(me, msg);
    return 0;
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
    if (strncmp(cmd, "MV", 2) == 0) return "19"; 
    if (strncmp(cmd, "RES", 3) == 0) return "23"; 
    if (strncmp(cmd, "DRW", 3) == 0) return "20"; 
    return SM_ACK;
}

int read_packet_wrapper(Client *me, char *readbuf, size_t *lp, char *linebuf, size_t linebuf_sz) {
    ssize_t r = recv(me->sock, readbuf, BUFFER_SZ, 0);
    if (r <= 0) return 0;

    me->last_heartbeat = time(NULL);

    for (ssize_t i = 0; i < r; ++i) {
        if (*lp + 1 < linebuf_sz) linebuf[(*lp)++] = readbuf[i];
        if (readbuf[i] == '\n') {
            linebuf[*lp] = '\0';
            trim_crlf(linebuf);
            *lp = 0;

            if (strcmp(linebuf, "PING") == 0) {
                send_line(me->sock, "PNG");
                continue; 
            }

            int seq = parse_seq_number(linebuf);
            strip_trailing_seq(linebuf);

            if (seq >= 0) {
                pthread_mutex_lock(&me->lock);
                me->seq = seq;
                pthread_mutex_unlock(&me->lock);
                if (me->state != STATE_HANDSHAKE) {
                    send_short_ack(me, ack_code_for_received(linebuf), seq);
                }
            }
            return 1; 
        }
    }
    return -1; 
}

int run_handshake(Client **me_ptr) {
    Client *me = *me_ptr;
    char readbuf[BUFFER_SZ];
    char linebuf[LINEBUF_SZ];
    size_t lp = 0;

    send_fmt_with_seq(me, "WELCOME ChessServer");

    while (me->state == STATE_HANDSHAKE) {
        int res = read_packet_wrapper(me, readbuf, &lp, linebuf, sizeof(linebuf));
        if (res == 0) return 0; 
        if (res < 0) continue; 

        if (strncmp(linebuf, "HELLO ", 6) == 0) {
            char name[NAME_LEN];
            strncpy(name, linebuf + 6, sizeof(name)-1);
            name[sizeof(name)-1] = '\0';

            Client *old_session = match_reconnect(name, me->sock);
            if (old_session) {
                log_printf("[CLIENT] Reconnect success. Swapping %p -> %p\n", me, old_session);
                pthread_mutex_destroy(&me->lock);
                
                /* [FIX] Close the temp socket struct but NOT the socket itself (transferred) */
                /* Actually match_reconnect transferred the FD to old_session. Safe to free struct. */
                free(me);
                
                me = old_session;
                *me_ptr = me;
                
                send_short_ack(me, HELLO_ACK, me->seq);
                match_try_resume(me->match);
                
                Client *opp = NULL;
                if (me->match) opp = (me->match->white == me) ? me->match->black : me->match->white;
                
                const char *opp_name = (opp && opp->name[0]) ? opp->name : "Unknown";
                const char *my_color = (me->color == 0) ? "white" : "black";
                
                send_fmt_with_seq(me, "RESUME %s %s", opp_name, my_color);
                
                if (opp && opp->sock > 0) {
                    const char *opp_color = (me->color == 0) ? "black" : "white";
                    send_fmt_with_seq(opp, "OPP_RESUME %s %s", me->name, opp_color);
                }
                
                if (me->match && me->match->moves_count > 0) {
                    char history[BIG_BUFFER_SZ] = "";
                    for(size_t j=0; j<me->match->moves_count; j++) {
                        if (strlen(history) + strlen(me->match->moves[j]) + 2 < sizeof(history)) {
                            strcat(history, me->match->moves[j]);
                            strcat(history, " ");
                        }
                    }
                    send_fmt_with_seq(me, "HISTORY %s", history);
                }
                
                pthread_mutex_lock(&me->match->lock);
                int rem = match_get_remaining_time(me->match);
                pthread_mutex_unlock(&me->match->lock);
                send_fmt_with_seq(me, "TIME %d", rem);
                if (opp && opp->sock > 0) send_fmt_with_seq(opp, "TIME %d", rem);

                if (me->match && !me->paired) me->state = STATE_WAITING;
                else me->state = STATE_GAME;
                
                log_printf("Resumed session for %s. State: %d\n", me->name, me->state);
                return 1;
            }

            snprintf(me->name, sizeof(me->name), "%s", name);
            send_short_ack(me, HELLO_ACK, me->seq);
            log_printf("Client identified as: %s\n", me->name);
            me->state = STATE_LOBBY;
            return 1;
        } else {
            if (handle_protocol_error(me, "Invalid protocol header")) return 0;
        }
    }
    return 1;
}

int run_lobby(Client *me) {
    char readbuf[BUFFER_SZ];
    char linebuf[LINEBUF_SZ];
    size_t lp = 0;

    me->match = NULL;
    me->paired = 0;
    me->color = -1;

    send_fmt_with_seq(me, "LOBBY (CMD: LIST, NEW, JOIN <id>)");

    while (me->state == STATE_LOBBY) {
        int res = read_packet_wrapper(me, readbuf, &lp, linebuf, sizeof(linebuf));
        if (res == 0) return 0;
        if (res < 0) continue;

        if (strcmp(linebuf, "LIST") == 0) {
            char *l = get_room_list_str();
            if (l) { send_fmt_with_seq(me, "ROOMLIST %s", l); free(l); }
        } 
        else if (strcmp(linebuf, "NEW") == 0) {
            Match *m = match_create(me);
            if (!m) send_error(me, "Server limit reached");
            else {
                me->match = m; 
                me->color = 0;
                send_fmt_with_seq(me, "WAITING Room %d", m->id);
                me->state = STATE_WAITING;
            }
        }
        else if (strncmp(linebuf, "JOIN ", 5) == 0) {
            int id = atoi(linebuf + 5);
            Match *m = find_open_room(id);
            if (!m) send_error(me, "Room full or closed");
            else {
                if (match_join(m, me) == 0) {
                    me->match = m; 
                    me->color = 1; 
                    me->paired = 1; 
                    m->white->paired = 1; 
                    notify_start(m);
                    me->state = STATE_GAME;
                } else send_error(me, "Join failed");
            }
        }
        else if (strcmp(linebuf, "EXT") == 0) { return 0; }
        else if (strncmp(linebuf, "MV", 2) == 0 || strncmp(linebuf, "RES", 3) == 0 || 
                 strncmp(linebuf, "DRW", 3) == 0 || strncmp(linebuf, "TIME", 4) == 0) {
             log_printf("Ignored stale game command from %s: %s\n", me->name, linebuf);
        }
        else {
            if (handle_protocol_error(me, "Unknown command")) return 0;
        }
    }
    return 1;
}

int run_waiting(Client *me) {
    char readbuf[BUFFER_SZ];
    char linebuf[LINEBUF_SZ];
    size_t lp = 0;

    while (me->state == STATE_WAITING) {
        if (me->paired && me->match) {
            me->state = STATE_GAME;
            return 1;
        }
        ssize_t r = recv(me->sock, readbuf, BUFFER_SZ, MSG_DONTWAIT);
        if (r > 0) {
            me->last_heartbeat = time(NULL);
            for (ssize_t i = 0; i < r; ++i) {
                if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
                if (readbuf[i] == '\n') {
                    linebuf[lp] = '\0';
                    trim_crlf(linebuf);
                    lp = 0;
                    if (strstr(linebuf, "EXT")) {
                        if (me->match) {
                            Match *m = me->match;
                            pthread_mutex_lock(&m->lock);
                            m->finished = 1;
                            m->white = NULL;
                            m->refs--;
                            int last = (m->refs <= 0);
                            pthread_mutex_unlock(&m->lock);
                            if (last) match_free(m);
                            me->match = NULL;
                            me->color = -1;
                        }
                        me->state = STATE_LOBBY;
                        return 1;
                    }
                    if (strcmp(linebuf, "PING") == 0) send_line(me->sock, "PNG");
                }
            }
        } else if (r == 0) { return 0; }
        usleep(100000); 
    }
    return 1;
}

int run_game(Client *me) {
    char readbuf[BUFFER_SZ];
    char linebuf[LINEBUF_SZ];
    size_t lp = 0;
    Match *myMatch = me->match;

    while (me->state == STATE_GAME) {
        int res = read_packet_wrapper(me, readbuf, &lp, linebuf, sizeof(linebuf));
        if (res == 0) return 0;
        if (res < 0) {
            if (myMatch && myMatch->finished) {
                me->state = STATE_LOBBY;
                return 1;
            }
            continue;
        }

        pthread_mutex_lock(&myMatch->lock);
        if (myMatch->finished) {
            pthread_mutex_unlock(&myMatch->lock);
            me->state = STATE_LOBBY;
            return 1;
        }

        /* ... [Game command parsing UNCHANGED] ... */
        if (strncmp(linebuf, "MV", 2) == 0) {
            if (myMatch->turn != me->color) {
                pthread_mutex_unlock(&myMatch->lock);
                if (handle_protocol_error(me, "Not your turn")) return 0;
            } else {
                char *mv = linebuf + 2;
                int r1, c1, r2, c2;
                if (!is_move_format(mv) || (parse_move(mv, &r1, &c1, &r2, &c2), 0) || 
                    !in_bounds(r1, c1) || !in_bounds(r2, c2) ||
                    !is_legal_move_basic(myMatch, me->color, r1, c1, r2, c2) ||
                    move_leaves_in_check(myMatch, me->color, r1, c1, r2, c2)) 
                {
                    pthread_mutex_unlock(&myMatch->lock);
                    if (handle_protocol_error(me, "Illegal Move")) return 0;
                } else {
                    char promo = (strlen(mv) >= 5) ? mv[4] : 0;
                    apply_move(myMatch, r1, c1, r2, c2, promo);
                    match_append_move(myMatch, mv);
                    send_fmt_with_seq(me, "OK_MV");
                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    if (opp && opp->sock > 0) send_fmt_with_seq(opp, "OPP_MV %s", mv);
                    int t = myMatch->turn_timeout_seconds;
                    send_fmt_with_seq(me, "TIME %d", t);
                    if (opp && opp->sock > 0) send_fmt_with_seq(opp, "TIME %d", t);
                    int opp_col = 1 - me->color;
                    int in_chk = is_in_check(&myMatch->state, opp_col);
                    int has_mv = has_any_legal_move(myMatch, opp_col);
                    if (in_chk && !has_mv) {
                        myMatch->finished = 1;
                        send_fmt_with_seq(me, "WIN_CHKM");
                        if (opp && opp->sock > 0) send_fmt_with_seq(opp, "CHKM");
                    } else if (!in_chk && !has_mv) {
                        myMatch->finished = 1;
                        send_fmt_with_seq(me, "SM");
                        if (opp && opp->sock > 0) send_fmt_with_seq(opp, "SM");
                    } else if (in_chk) {
                        if (opp && opp->sock > 0) send_fmt_with_seq(opp, "CHK");
                    }
                    if (!myMatch->finished) {
                        myMatch->turn = 1 - myMatch->turn;
                        myMatch->last_move_time = time(NULL);
                    }
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
        }
        else if (strncmp(linebuf, "DRW_OFF", 7) == 0) {
             Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
             if (opp && opp->sock > 0) send_fmt_with_seq(opp, "DRW_OFF");
             myMatch->draw_offered_by = me->color;
             pthread_mutex_unlock(&myMatch->lock);
        }
        else if (strncmp(linebuf, "DRW_ACC", 7) == 0) {
            Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
            if (myMatch->draw_offered_by != opp->color) { 
                pthread_mutex_unlock(&myMatch->lock); continue;
            }
             myMatch->finished = 1;
             send_fmt_with_seq(me, "DRW_ACD");
             if (opp && opp->sock > 0) send_fmt_with_seq(opp, "DRW_ACD");
             pthread_mutex_unlock(&myMatch->lock);
        }
        else if (strncmp(linebuf, "DRW_DEC", 7) == 0) {
             Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
             if (opp && opp->sock > 0) send_fmt_with_seq(opp, "DRW_DCD");
             myMatch->draw_offered_by = -1;
             pthread_mutex_unlock(&myMatch->lock);
        }
        else if (strncmp(linebuf, "EXT", 3) == 0) {
            myMatch->finished = 1;
            Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
            if (opp) send_fmt_with_seq(opp, "OPP_EXT");
            pthread_mutex_unlock(&myMatch->lock);
        }
        else if (strncmp(linebuf, "LIST", 4) == 0 || strncmp(linebuf, "JOIN", 4) == 0 || strncmp(linebuf, "NEW", 3) == 0) {
             pthread_mutex_unlock(&myMatch->lock);
             log_printf("Ignored stale lobby command in game: %s\n", linebuf);
        }
        else {
            pthread_mutex_unlock(&myMatch->lock);
            if (handle_protocol_error(me, "Unknown game command")) return 0;
        }
        if (myMatch && myMatch->finished) me->state = STATE_LOBBY;
    }
    return 1;
}

void *client_worker(void *arg) {
    Client *me = (Client *)arg;
    unsigned int seed = time(NULL) ^ (uintptr_t)me ^ me->sock;
    me->seq = rand_r(&seed) & 0x1FF;

    log_printf("[CLIENT %p] Worker started. Sock=%d.\n", me, me->sock);
    
    pthread_mutex_lock(&players_lock);
    current_players++;
    pthread_mutex_unlock(&players_lock);

    me->state = STATE_HANDSHAKE;
    me->last_heartbeat = time(NULL);

    while (me->state != STATE_DISCONNECTED) {
        int keep_alive = 0;
        switch (me->state) {
            case STATE_HANDSHAKE: keep_alive = run_handshake(&me); break;
            case STATE_LOBBY: keep_alive = run_lobby(me); break;
            case STATE_WAITING: keep_alive = run_waiting(me); break;
            case STATE_GAME: keep_alive = run_game(me); break;
            default: keep_alive = 0; break;
        }
        if (!keep_alive) me->state = STATE_DISCONNECTED;
    }

    log_printf("[CLIENT %p] Entering disconnect sequence.\n", me);
    int persisted = match_release_after_client(me);
    
    if (persisted) {
        log_printf("[CLIENT %p] Client thread exited (Persisted). Closing sock %d.\n", me, me->sock);
        /* [FIX] Close socket even if persisting logic state, to free FD */
        if (me->sock > 0) { close(me->sock); me->sock = -1; }
    } else {
        log_printf("[CLIENT %p] Client thread exited (Freeing).\n", me);
        if (me->sock > 0) close(me->sock);
        if (me) pthread_mutex_destroy(&me->lock);
        free(me);
    }
    
    pthread_mutex_lock(&players_lock);
    current_players--;
    pthread_mutex_unlock(&players_lock);
    return NULL;
}