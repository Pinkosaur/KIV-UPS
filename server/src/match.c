/* match.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h> 
#include "match.h"
#include "client.h"
#include "game.h"
#include "logging.h"

extern int max_rooms;

/* Global Room Registry */
static Match *global_room_list = NULL;
static int next_room_id = 1;
static int current_room_count = 0;
static pthread_mutex_t room_registry_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Room Registry Helpers --- */

void register_room(Match *m) {
    pthread_mutex_lock(&room_registry_lock);
    m->id = next_room_id++;
    m->next = global_room_list;
    global_room_list = m;
    current_room_count++;
    pthread_mutex_unlock(&room_registry_lock);
}

void unregister_room(Match *m) {
    pthread_mutex_lock(&room_registry_lock);
    Match **curr = &global_room_list;
    while (*curr) {
        if (*curr == m) {
            *curr = m->next;
            current_room_count--;
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&room_registry_lock);
}

Match *find_open_room(int id) {
    pthread_mutex_lock(&room_registry_lock);
    Match *curr = global_room_list;
    while (curr) {
        if (curr->id == id && curr->black == NULL && !curr->finished) {
            pthread_mutex_unlock(&room_registry_lock);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&room_registry_lock);
    return NULL;
}

char *get_room_list_str() {
    pthread_mutex_lock(&room_registry_lock);
    char *buf = malloc(4096);
    if (!buf) { pthread_mutex_unlock(&room_registry_lock); return NULL; }
    buf[0] = '\0';
    size_t len = 0; 
    
    Match *curr = global_room_list;
    int count = 0;
    while (curr) {
        if (curr->black == NULL && !curr->finished) {
            char line[128];
            snprintf(line, sizeof(line), "%d:%s ", curr->id, curr->white->name);
            
            size_t line_len = strlen(line);
            if (len + line_len < 4095) { 
                strcat(buf, line);
                len += line_len;
            }
            count++;
        }
        curr = curr->next;
    }
    if (count == 0) strcpy(buf, "EMPTY");
    pthread_mutex_unlock(&room_registry_lock);
    return buf;
}

Match *match_create(Client *white) {
    Match *m = calloc(1, sizeof(Match));
    if (!m) return NULL;
    
    m->white = white;
    m->black = NULL; 
    m->turn = 0; 
    pthread_mutex_init(&m->lock, NULL);
    m->moves = NULL;
    m->moves_count = 0;
    m->finished = 0;   
    m->draw_offered_by = -1;

    m->w_can_kingside = 1; m->w_can_queenside = 1;
    m->b_can_kingside = 1; m->b_can_queenside = 1;
    m->ep_r = -1; m->ep_c = -1;

    init_board(&m->state); 

    m->last_move_time = 0; 
    m->turn_timeout_seconds = TURN_TIMEOUT_SECONDS;
    
    m->refs = 2; 

    register_room(m);

    pthread_t wt;
    if (pthread_create(&wt, NULL, match_watchdog, m) == 0) {
        pthread_detach(wt);
    } else {
        m->refs = 1; 
    }

    return m;
}

int match_join(Match *m, Client *black) {
    pthread_mutex_lock(&m->lock);
    if (m->black != NULL || m->finished) {
        pthread_mutex_unlock(&m->lock);
        return -1; 
    }
    m->black = black;
    m->last_move_time = time(NULL);
    m->refs++; 
    pthread_mutex_unlock(&m->lock);
    return 0;
}

void match_free(Match *m) {
    if (!m) return;
    unregister_room(m);
    for (size_t i = 0; i < m->moves_count; ++i) free(m->moves[i]);
    free(m->moves);
    
    if (m->white && m->white->sock > 0) close(m->white->sock);
    if (m->black && m->black->sock > 0) close(m->black->sock);

    pthread_mutex_destroy(&m->lock);
    free(m);
}

/* Returns 1 if client persisted (DO NOT FREE), 0 if released (SAFE TO FREE) */
int match_release_after_client(Client *me) {
    if (!me) return 0;
    if (!me->match) {
        log_printf("[MATCH] Release %p: No match attached. Safe to free.\n", me);
        return 0; 
    }
    Match *m = me->match;
    Client *other = (me == m->white) ? m->black : m->white;

    pthread_mutex_lock(&m->lock);
    
    /* CASE 1: Game Finished / Player Quitting (Not Reconnecting) */
    if (m->finished) {
        if (me == m->white) m->white = NULL;
        else if (me == m->black) m->black = NULL;

        me->match = NULL;
        m->refs--;
        int last = (m->refs <= 0);
        
        log_printf("[MATCH] Release client %p (Quit/Finished). Refs=%d. Destroy=%d\n", me, m->refs, last);
        pthread_mutex_unlock(&m->lock);
        
        if (last) match_free(m);
        return 0; /* 0 = Thread should free(me) */
    }

    /* CASE 2: Game ACTIVE. Disconnect/Timeout. Persist Session. */
    log_printf("[MATCH] Client %p (%s) disconnected. Keeping session alive. Sock=-1. (Persist=1)\n", me, me->name);
    
    me->sock = -1; 
    me->disconnect_time = time(NULL);
    
    /* Notify opponent */
    if (other && other->sock > 0) {
        send_fmt_with_seq(other, "WAIT_CONN"); 
    }

    pthread_mutex_unlock(&m->lock);
    return 1; /* 1 = Persisted. Thread must NOT free(me) */
}

Client *match_reconnect(const char *name, int new_sock) {
    pthread_mutex_lock(&room_registry_lock);
    Match *curr = global_room_list;
    while (curr) {
        pthread_mutex_lock(&curr->lock);
        if (!curr->finished) {
            Client *target = NULL;
            if (curr->white && strcmp(curr->white->name, name) == 0 && curr->white->sock == -1) {
                target = curr->white;
            } else if (curr->black && strcmp(curr->black->name, name) == 0 && curr->black->sock == -1) {
                target = curr->black;
            }

            if (target) {
                target->sock = new_sock;
                target->disconnect_time = 0;
                target->seq = 0; 
                
                /* FIX: Reset heartbeat timer so Watchdog doesn't immediately kill the reconnected session */
                target->last_heartbeat = time(NULL);

                log_printf("[MATCH] Client %p (%s) RECONNECTED to match %d.\n", target, name, curr->id);
                pthread_mutex_unlock(&curr->lock);
                pthread_mutex_unlock(&room_registry_lock);
                return target;
            }
        }
        pthread_mutex_unlock(&curr->lock);
        curr = curr->next;
    }
    pthread_mutex_unlock(&room_registry_lock);
    return NULL;
}

int match_append_move(Match *m, const char *mv) {
    if (!m || !mv) return -1;
    if (m->moves_count + 1 > m->moves_cap) {
        size_t newcap = (m->moves_cap == 0) ? 8 : m->moves_cap * 2;
        char **tmp = realloc(m->moves, newcap * sizeof(char *));
        if (!tmp) return -1;
        m->moves = tmp;
        m->moves_cap = newcap;
    }
    m->moves[m->moves_count++] = strdup(mv);
    return 0;
}

void notify_start(Match *m) {
    if (!m || !m->white || !m->black) return;
    send_fmt_with_seq(m->white, "START %s white", m->black->name);
    send_fmt_with_seq(m->black, "START %s black", m->white->name);
}

void *match_watchdog(void *arg) {
    Match *m = (Match *)arg;
    if (!m) return NULL;

    while (1) {
        sleep(1); 

        pthread_mutex_lock(&m->lock);

        if (m->finished) {
            m->refs--; 
            int last = (m->refs <= 0);
            pthread_mutex_unlock(&m->lock);
            
            if (last) match_free(m); 
            break; 
        }

        time_t now = time(NULL);

        /* 1. Game Move Timeout */
        int timed_out = 0;
        if (m->last_move_time != 0 && (now - m->last_move_time) >= m->turn_timeout_seconds) {
            timed_out = 1;
        }
        if (timed_out) {
            Client *inactive = (m->turn == 0) ? m->white : m->black;
            Client *winner   = (m->turn == 0) ? m->black : m->white;
            m->finished = 1; 
            if (inactive && inactive->sock > 0) send_line(inactive->sock, "TOUT");
            if (winner && winner->sock > 0) send_line(winner->sock, "OPP_TOUT");
            pthread_mutex_unlock(&m->lock);
            continue; 
        }

        /* 2. ZOMBIE CHECK (Heartbeat Timeout) */
        if (m->white && m->white->sock > 0 && (now - m->white->last_heartbeat > 15)) {
            log_printf("[WATCHDOG] Client %p (%s) timed out (no heartbeat). Closing socket.\n", m->white, m->white->name);
            shutdown(m->white->sock, SHUT_RDWR);
            close(m->white->sock);
            m->white->sock = -1; 
            m->white->disconnect_time = now;
        }
        if (m->black && m->black->sock > 0 && (now - m->black->last_heartbeat > 15)) {
            log_printf("[WATCHDOG] Client %p (%s) timed out (no heartbeat). Closing socket.\n", m->black, m->black->name);
            shutdown(m->black->sock, SHUT_RDWR);
            close(m->black->sock);
            m->black->sock = -1; 
            m->black->disconnect_time = now;
        }

        /* 3. Disconnect Timeout - Explicit Check */
        int w_dc = 0;
        if (m->white && m->white->sock == -1) {
             if (now - m->white->disconnect_time > DISCONNECT_TIMEOUT_SECONDS) w_dc = 1;
        }
        
        int b_dc = 0;
        if (m->black && m->black->sock == -1) {
             if (now - m->black->disconnect_time > DISCONNECT_TIMEOUT_SECONDS) b_dc = 1;
        }

        if (w_dc || b_dc) {
            log_printf("[WATCHDOG] Match %d forfeited due to disconnection timeout.\n", m->id);
            m->finished = 1; 
            Client *winner = w_dc ? m->black : m->white; 
            if (winner && winner->sock > 0) send_line(winner->sock, "OPP_EXT");
            
            if (w_dc) m->refs--;
            if (b_dc) m->refs--;

            pthread_mutex_unlock(&m->lock);
            continue;
        }

        pthread_mutex_unlock(&m->lock);
    }
    return NULL;
}