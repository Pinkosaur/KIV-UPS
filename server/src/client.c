/* src/client.c */
/**
 * @file client.c
 * @brief Implementation of client thread management and protocol state machine.
 *
 * This file handles the lifecycle of a client connection, including the initial
 * handshake, lobby interactions, matchmaking, and the gameplay loop. It manages
 * concurrency via player counting and strictly adheres to the defined text-based protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h> 
#include "client.h"
#include "match.h"
#include "game.h"
#include "logging.h"
#include "config.h"

extern int max_players;
extern int max_rooms;

/**
 * Tracks the number of currently connected players (TCP connections or persisted sessions).
 * Protected by players_lock.
 */
static int current_players = 0;
static pthread_mutex_t players_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Retrieves the current number of online players in a thread-safe manner.
 * @return The current player count.
 */
int get_online_players(void) {
    int count;
    pthread_mutex_lock(&players_lock);
    count = current_players;
    pthread_mutex_unlock(&players_lock);
    return count;
}

/**
 * @brief Increment the global player count.
 */
void increment_player_count(void) {
    pthread_mutex_lock(&players_lock);
    current_players++;
    pthread_mutex_unlock(&players_lock);
}

/**
 * @brief Decrement the global player count.
 * Safe to call even if count is zero (prevents underflow).
 */
void decrement_player_count(void) {
    pthread_mutex_lock(&players_lock);
    if (current_players > 0) current_players--;
    pthread_mutex_unlock(&players_lock);
}

/**
 * @brief Atomically checks if there is space for a new player and increments the count.
 * @return 1 if a slot was reserved, 0 if the server is full.
 */
int try_reserve_slot(void) {
    int success = 0;
    pthread_mutex_lock(&players_lock);
    if (max_players <= 0 || current_players < max_players) {
        current_players++;
        success = 1;
    }
    pthread_mutex_unlock(&players_lock);
    return success;
}

/* --- Protocol Helpers --- */

/**
 * @brief Sends raw bytes to a socket, ignoring SIGPIPE.
 * @param sock The file descriptor of the socket.
 * @param msg The null-terminated string to send.
 */
void send_raw(int sock, const char *msg) {
    if (sock > 0) send(sock, msg, strlen(msg), MSG_NOSIGNAL);
}

/**
 * @brief Sends a string followed by a newline character to the socket.
 * @param sock The file descriptor of the socket.
 * @param msg The message string.
 */
void send_line(int sock, const char *msg) {
    if (sock <= 0) return;
    char tmp[BUFFER_SZ];
    snprintf(tmp, sizeof(tmp), "%s\n", msg);
    send_raw(sock, tmp);
}

/**
 * @brief Formats and sends a protocol message to the client.
 * This function performs thread-safe socket writing and logs the communication.
 *
 * @param c Pointer to the Client structure.
 * @param fmt Printf-style format string.
 * @param ... Arguments for the format string.
 */
void send_protocol_msg(Client *c, const char *fmt, ...) {
    if (!c || c->sock <= 0) return;
    char payload[BIG_BUFFER_SZ];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(payload, sizeof(payload), fmt, ap);
    va_end(ap);

    char out[BIG_BUFFER_SZ + 2];
    snprintf(out, sizeof(out), "%s\n", payload);

    pthread_mutex_lock(&c->lock);
    send_raw(c->sock, out);
    pthread_mutex_unlock(&c->lock);
    
    log_printf("SENT -> %s (sock %d) : %s", c->name[0] ? c->name : "unknown", c->sock, out);
}

/**
 * @brief Sends a short acknowledgement code to the client.
 *
 * @param c Pointer to the Client structure.
 * @param ack_code The protocol ACK code.
 */
void send_short_ack(Client *c, const char *ack_code) {
    if (!c || c->sock <= 0) return;
    send_line(c->sock, ack_code);
}

/**
 * @brief Sends a formatted error message to the client.
 * @param c Pointer to the Client structure.
 * @param reason Description of the error.
 */
void send_error(Client *c, const char *reason) {
    send_protocol_msg(c, "ERR %s", reason);
}

/**
 * @brief Logs a protocol error and checks if the client has exceeded the error threshold.
 * * If the error count exceeds MAX_ERRORS, the client is disconnected and any active match is forfeited.
 *
 * @param me Pointer to the Client structure.
 * @param msg Error description.
 * @return 1 if the client was disconnected/kicked, 0 otherwise.
 */
int handle_protocol_error(Client *me, const char *msg) {
    me->error_count++;
    log_printf("[CLIENT %s] Protocol Error %d/%d: %s\n", me->name, me->error_count, MAX_ERRORS, msg);
    
    if (me->error_count >= MAX_ERRORS) {
        send_error(me, "Too many invalid messages. Disconnecting.");
        if (me->match) {
            pthread_mutex_lock(&me->match->lock);
            if (!me->match->finished) {
                Client *opp = (me->match->white == me) ? me->match->black : me->match->white;
                if (opp && opp->sock > 0) {
                    send_protocol_msg(opp, OPPONENT_KICKED_OUT);
                }
                me->match->finished = 1; 
            }
            pthread_mutex_unlock(&me->match->lock);
        }
        return 1; 
    }
    send_error(me, msg);
    return 0;
}

/**
 * @brief Removes trailing carriage return and newline characters.
 */
void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

/**
 * @brief Determines the appropriate ACK code for a received command.
 * Maps every known protocol command to a numeric acknowledgement code.
 * * @param cmd The command string.
 * @return String literal representing the ACK code.
 */
const char *ack_code_for_received(const char *cmd) {
    /* Handshake */
    if (strncmp(cmd, HELLO, 5) == 0)             return HELLO_ACK;
    
    /* Lobby & Room Management */
    if (strncmp(cmd, ENTER_LOBBY, 5) == 0)       return LOBBY_ACK;
    if (strncmp(cmd, ROOM_LIST_REQUEST, 4) == 0) return LIST_REQ_ACK;
    if (strncmp(cmd, CREATE_ROOM, 3) == 0)       return NEW_ROOM_ACK;
    if (strncmp(cmd, JOIN_ROOM, 4) == 0)         return JOIN_REQ_ACK;
    
    /* Gameplay Commands */
    if (strncmp(cmd, MOVE_COMMAND, 2) == 0)      return MOVE_COMMAND_ACK;
    if (strncmp(cmd, RESIGN, 3) == 0)            return RESIGN_ACK_CS;
    if (strncmp(cmd, DRAW_OFFER, 7) == 0)        return DRAW_OFFER_ACK_CS;
    if (strncmp(cmd, ACCEPT_DRAW, 7) == 0)       return ACCEPT_DRAW_ACK;
    if (strncmp(cmd, DECLINE_DRAW, 7) == 0)      return DECLINE_DRAW_ACK;
    
    /* System */
    if (strncmp(cmd, EXIT, 3) == 0)              return EXIT_ACK;
    
    return GENERIC_ACK;
}

/**
 * @brief Wrapper for reading packets that handles fragmentation and buffering.
 * * This function reads from the socket into a persistence buffer, extracts complete lines,
 * and handles automatic PING/PONG responses and ACK consumption.
 *
 * @param me Client structure.
 * @param readbuf Raw buffer for recv calls.
 * @param b_start Pointer to current start index in readbuf.
 * @param b_len Pointer to current length of valid data in readbuf.
 * @param lp Pointer to current length of linebuf.
 * @param linebuf Buffer to assemble the current line.
 * @param linebuf_sz Size of linebuf.
 * @param recv_flags Flags for recv (e.g., MSG_DONTWAIT).
 * @return 1 on successful line read, 0 on disconnect, -1 on fatal error, -2 on would block/retry.
 */
int read_packet_wrapper(Client *me, char *readbuf, int *b_start, int *b_len, size_t *lp, char *linebuf, size_t linebuf_sz, int recv_flags) {
    while (1) {
        if (*b_start >= *b_len) {
            *b_start = 0;
            *b_len = recv(me->sock, readbuf, BUFFER_SZ, recv_flags);
            if (*b_len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -2;
                return -1; 
            }
            if (*b_len == 0) return 0;
            me->last_heartbeat = time(NULL);
        }

        while (*b_start < *b_len) {
            char c = readbuf[(*b_start)++];
            if (*lp + 1 < linebuf_sz) linebuf[(*lp)++] = c;
            if (c == '\n') {
                linebuf[*lp] = '\0';
                trim_crlf(linebuf);
                *lp = 0; 
                if (strlen(linebuf) == 0) continue;
                
                // PING handling
                if (strcmp(linebuf, PING) == 0) {
                    send_line(me->sock, PING_RESPONSE);
                    continue; 
                }

                // Handle Client ACKs (2-digit codes)
                // We consume them here to prevent them from being treated as unknown commands.
                // The heartbeat is implicitly updated by the recv call above.
                if (strlen(linebuf) == 2 && isdigit(linebuf[0]) && isdigit(linebuf[1])) {
                    log_printf("[CLIENT %s] ACK RX: %s\n", me->name[0] ? me->name : "unknown", linebuf);
                    continue;
                }

                /* Send ACK for every valid command received (except during handshake where logic is explicit) */
                if (me->state != STATE_HANDSHAKE) {
                    send_short_ack(me, ack_code_for_received(linebuf));
                }
                return 1; 
            }
        }
        if (recv_flags & MSG_DONTWAIT) return -2; 
    }
}

/**
 * @brief Sends a rejection message and closes the socket.
 * Used when the server is full or errors occur during connection.
 */
void reject_connection(int sock) {
    const char *msg = PLAYER_LIMIT_REACHED;
    send(sock, msg, strlen(msg), MSG_NOSIGNAL);
    usleep(300000); // wait for the message to be sent entirely before closing
    close(sock);
}

/**
 * @brief Handles the client handshake phase.
 * * Processes the HELLO command, handles reconnections for disconnected sessions,
 * and enforces the server player limit for new connections.
 *
 * @param me_ptr Double pointer to the client object (may be replaced on reconnect).
 * @return 1 to transition to the next state, 0 to disconnect.
 */
int run_handshake(Client **me_ptr) {
    Client *me = *me_ptr;
    char readbuf[BUFFER_SZ]; char linebuf[LINEBUF_SZ]; size_t lp = 0;
    int b_start = 0, b_len = 0; 

    send_protocol_msg(me, WELCOME);

    while (me->state == STATE_HANDSHAKE) {
        int res = read_packet_wrapper(me, readbuf, &b_start, &b_len, &lp, linebuf, sizeof(linebuf), 0);
        if (res == 0 || res == -1) return 0;
        if (res == -2) continue; 

        if (strncmp(linebuf, HELLO, 6) == 0) {
            char name[NAME_LEN]; char id[ID_LEN];
            int args = sscanf(linebuf + 6, "%63s %31s", name, id);
            if (args < 1) continue; 
            if (args < 2) strncpy(id, "unknown", sizeof(id));

            Client *old_session = match_reconnect(name, id, me->sock);
            if (old_session) {
                pthread_mutex_destroy(&me->lock);
                free(me);
                me = old_session;
                *me_ptr = me;
                send_short_ack(me, HELLO_ACK);
                match_try_resume(me->match);
                if (me->match && !me->paired) {
                    me->state = STATE_WAITING;
                    send_protocol_msg(me, WAIT, me->match->id);
                } else {
                    me->state = STATE_GAME;
                    Client *opp = (me->match->white == me) ? me->match->black : me->match->white;
                    send_protocol_msg(me, RESUME_MATCH, (opp&&opp->name[0])?opp->name:"Unknown", (me->color==0)?"white":"black");
                    if (opp && opp->sock > 0) send_protocol_msg(opp, OPPONENT_RETURNED, me->name, (me->color==0)?"black":"white");
                    if (me->match && me->match->moves_count > 0) {
                        char history[BIG_BUFFER_SZ] = "";
                        for(size_t j=0; j<me->match->moves_count; j++) {
                            strcat(history, me->match->moves[j]);
                            strcat(history, " ");
                        }
                        send_protocol_msg(me, MATCH_HISTORY, history);
                    }
                    pthread_mutex_lock(&me->match->lock);
                    int rem = match_get_remaining_time(me->match);
                    pthread_mutex_unlock(&me->match->lock);
                    send_protocol_msg(me, TURN_TIMER_STATE, rem);
                    if (opp && opp->sock > 0) send_protocol_msg(opp, TURN_TIMER_STATE, rem);
                }
                return 1;
            }

            if (!try_reserve_slot()) {
                reject_connection(me->sock); 
                return 0;
            }
            me->is_counted = 1; 
            snprintf(me->name, sizeof(me->name), "%s", name);
            snprintf(me->id, sizeof(me->id), "%s", id);
            send_short_ack(me, HELLO_ACK);
            me->state = STATE_LOBBY;
            return 1;
        } else {
            if (handle_protocol_error(me, "Invalid protocol header")) return 0;
        }
    }
    return 1;
}

/**
 * @brief Handles the lobby state.
 * Allows clients to list rooms, create new rooms, or join existing ones.
 */
int run_lobby(Client *me) {
    char readbuf[BUFFER_SZ]; char linebuf[LINEBUF_SZ]; size_t lp = 0;
    int b_start = 0, b_len = 0; 
    me->match = NULL; me->paired = 0; me->color = -1;
    send_protocol_msg(me, ENTER_LOBBY);
    while (me->state == STATE_LOBBY) {
        int res = read_packet_wrapper(me, readbuf, &b_start, &b_len, &lp, linebuf, sizeof(linebuf), 0);
        if (res == 0 || res == -1) return 0;
        if (res == -2) continue;
        if (strcmp(linebuf, ROOM_LIST_REQUEST) == 0) {
            char *l = get_room_list_str();
            if (l) { send_protocol_msg(me, ROOM_LIST_ANSWER, l); free(l); }
        } 
        else if (strcmp(linebuf, CREATE_ROOM) == 0) {
            if (max_rooms > 0 && get_active_room_count() >= max_rooms) {
                send_error(me, "Server room limit reached");
            } else {
                Match *m = match_create(me);
                if (!m) send_error(me, "Server internal limit reached");
                else {
                    me->match = m; me->color = 0;
                    send_protocol_msg(me, WAIT, m->id);
                    me->state = STATE_WAITING;
                }
            }
        }
        else if (strncmp(linebuf, JOIN_ROOM, 5) == 0) {
            int id = atoi(linebuf + 5);
            if (match_join_by_id(id, me) == 0) {
                Match *m = me->match; 
                me->color = 1; me->paired = 1; 
                if (m->white) m->white->paired = 1; 
                notify_start(m);
                me->state = STATE_GAME;
            } else send_error(me, "Room full or closed");
        }
        else if (strcmp(linebuf, EXIT) == 0) return 0;
        else if (handle_protocol_error(me, "Unknown command")) return 0;
    }
    return 1;
}

/**
 * @brief Handles the waiting state for a room host.
 * Waits for an opponent to join or for the host to cancel creation.
 */
int run_waiting(Client *me) {
    char readbuf[BUFFER_SZ]; char linebuf[LINEBUF_SZ]; size_t lp = 0;
    int b_start = 0, b_len = 0; 
    while (me->state == STATE_WAITING) {
        if (me->paired && me->match) { me->state = STATE_GAME; return 1; }
        int res = read_packet_wrapper(me, readbuf, &b_start, &b_len, &lp, linebuf, sizeof(linebuf), MSG_DONTWAIT);
        if (res > 0 && strstr(linebuf, EXIT)) {
            if (me->match) {
                Match *m = me->match; pthread_mutex_lock(&m->lock);
                m->finished = 1; m->white = NULL; m->refs--; int last = (m->refs <= 0);
                pthread_mutex_unlock(&m->lock); if (last) match_free(m);
                me->match = NULL; me->color = -1;
            }
            me->state = STATE_LOBBY; return 1;
        } 
        else if (res == 0 || res == -1) return 0;
        usleep(100000); 
    }
    return 1;
}

/**
 * @brief Handles the main gameplay state.
 * Processes moves, resignations, draw offers, and game termination.
 */
int run_game(Client *me) {
    char readbuf[BUFFER_SZ]; char linebuf[LINEBUF_SZ]; size_t lp = 0;
    int b_start = 0, b_len = 0; Match *myMatch = me->match;
    while (me->state == STATE_GAME) {
        int res = read_packet_wrapper(me, readbuf, &b_start, &b_len, &lp, linebuf, sizeof(linebuf), 0);
        if (res == 0 || res == -1) return 0;
        if (res == -2) continue; 
        pthread_mutex_lock(&myMatch->lock);
        if (myMatch->finished) {
            pthread_mutex_unlock(&myMatch->lock);
            match_leave_by_client(me);
            me->state = STATE_LOBBY; return 1;
        }
        if (strncmp(linebuf, MOVE_COMMAND, 2) == 0) {
            if (myMatch->turn != me->color) {
                pthread_mutex_unlock(&myMatch->lock);
                if (handle_protocol_error(me, "Not your turn")) return 0;
            } else {
                char *mv = linebuf + 2; int r1, c1, r2, c2;
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
                    send_protocol_msg(me, ACCEPT_MOVE);
                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    if (opp && opp->sock > 0) send_protocol_msg(opp, OPPONENT_MOVE, mv);
                    int t = myMatch->turn_timeout_seconds;
                    send_protocol_msg(me, TURN_TIMER_STATE, t);
                    if (opp && opp->sock > 0) send_protocol_msg(opp, TURN_TIMER_STATE, t);
                    int opp_col = 1 - me->color;
                    int in_chk = is_in_check(&myMatch->state, opp_col);
                    int has_mv = has_any_legal_move(myMatch, opp_col);
                    if (in_chk && !has_mv) {
                        myMatch->finished = 1; send_protocol_msg(me, WON_BY_CHECKMATE);
                        if (opp && opp->sock > 0) send_protocol_msg(opp, LOST_BY_CHECKMATE);
                    } else if (!in_chk && !has_mv) {
                        myMatch->finished = 1; send_protocol_msg(me, STALEMATE);
                        if (opp && opp->sock > 0) send_protocol_msg(opp, STALEMATE);
                    } else if (in_chk && opp && opp->sock > 0) send_protocol_msg(opp, IN_CHECK);
                    if (!myMatch->finished) { myMatch->turn = 1 - myMatch->turn; myMatch->last_move_time = time(NULL); }
                }
                pthread_mutex_unlock(&myMatch->lock);
            }
        }
        else if (strncmp(linebuf, RESIGN, 3) == 0) {
            myMatch->finished = 1; send_protocol_msg(me, YOU_RESIGNED);
            Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
            if (opp && opp->sock > 0) send_protocol_msg(opp, OPPONENT_RESIGNED);
            pthread_mutex_unlock(&myMatch->lock);
        }
        else if (strncmp(linebuf, DRAW_OFFER, 7) == 0) {
             Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
             if (opp && opp->sock > 0) send_protocol_msg(opp, DRAW_OFFER);
             myMatch->draw_offered_by = me->color;
             pthread_mutex_unlock(&myMatch->lock);
        }
        else if (strncmp(linebuf, ACCEPT_DRAW, 7) == 0) {
             Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
             myMatch->finished = 1; send_protocol_msg(me, DRAW_ACCEPTED);
             if (opp && opp->sock > 0) send_protocol_msg(opp, DRAW_ACCEPTED);
             pthread_mutex_unlock(&myMatch->lock);
        }
        else if (strncmp(linebuf, DECLINE_DRAW, 7) == 0) {
             Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
             if (opp && opp->sock > 0) send_protocol_msg(opp, DRAW_DECLINED);
             myMatch->draw_offered_by = -1;
             pthread_mutex_unlock(&myMatch->lock);
        }
        else if (strncmp(linebuf, EXIT, 3) == 0) {
            myMatch->finished = 1;
            Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
            if (opp) send_protocol_msg(opp, OPPONENT_QUIT);
            pthread_mutex_unlock(&myMatch->lock);
        }
        else { pthread_mutex_unlock(&myMatch->lock); if (handle_protocol_error(me, "Unknown command")) return 0; }
        if (myMatch && myMatch->finished) { match_leave_by_client(me); me->state = STATE_LOBBY; return 1; }
    }
    return 1;
}

/**
 * @brief Main entry point for a client thread.
 * * Initializes the client state and executes the Finite State Machine (FSM)
 * loop until disconnection. Handles cleanup upon exit.
 */
void *client_worker(void *arg) {
    Client *me = (Client *)arg;
    log_printf("[CLIENT %p] Worker started. Sock=%d.\n", me, me->sock);
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
    int sock_to_close = me->sock;
    int persisted = match_release_after_client(me);
    if (!persisted) {
        if (sock_to_close > 0) close(sock_to_close);
        if (me) { if (me->is_counted) decrement_player_count(); pthread_mutex_destroy(&me->lock); }
        free(me);
    } else if (sock_to_close > 0) close(sock_to_close);
    return NULL;
}