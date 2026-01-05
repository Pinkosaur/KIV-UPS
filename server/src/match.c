/* src/match.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h> 
#include <stdint.h>
#include "match.h"
#include "client.h"
#include "game.h"
#include "logging.h"
#include "config.h"

extern int max_rooms;

/* Global Room Registry */
static Match *global_room_list = NULL;
static int next_room_id = 1;
static int current_room_count = 0;
static pthread_mutex_t room_registry_lock = PTHREAD_MUTEX_INITIALIZER;

/* Thread-safe accessor */
int get_active_room_count(void) {
    int count;
    pthread_mutex_lock(&room_registry_lock);
    count = current_room_count;
    pthread_mutex_unlock(&room_registry_lock);
    return count;
}

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

void match_leave_by_client(Client *me) {
    if (!me || !me->match) return;
    Match *m = me->match;
    
    pthread_mutex_lock(&m->lock);
    
    if (m->white == me) m->white = NULL;
    else if (m->black == me) m->black = NULL;
    
    if (m->refs > 0) m->refs--;
    int last = (m->refs <= 0);
    
    pthread_mutex_unlock(&m->lock);
    
    me->match = NULL;
    me->paired = 0;
    me->color = -1;
    
    if (last) match_free(m);
}

int match_join_by_id(int id, Client *black) {
    pthread_mutex_lock(&room_registry_lock);
    Match *curr = global_room_list;
    Match *target = NULL;
    while (curr) {
        if (curr->id == id) {
            target = curr;
            target->refs++; 
            break;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&room_registry_lock);

    if (!target) return -1; 

    pthread_mutex_lock(&target->lock);
    if (target->black != NULL || target->finished) {
        target->refs--; 
        pthread_mutex_unlock(&target->lock);
        return -1; 
    }

    target->black = black;
    black->match = target; 
    target->last_move_time = time(NULL);
    
    pthread_mutex_unlock(&target->lock);
    return 0;
}

char *get_room_list_str() {
    pthread_mutex_lock(&room_registry_lock);
    char *buf = malloc(BIG_BUFFER_SZ);
    if (!buf) { pthread_mutex_unlock(&room_registry_lock); return NULL; }
    
    char *ptr = buf;
    char *end = buf + BIG_BUFFER_SZ;
    *ptr = '\0';
    
    Match *curr = global_room_list;
    int count = 0;
    while (curr) {
        if (curr->black == NULL && !curr->finished) {
            int remaining = end - ptr;
            if (remaining > 1) {
                int written = snprintf(ptr, remaining, "%d:%s ", curr->id, curr->white->name);
                if (written > 0 && written < remaining) {
                    ptr += written;
                }
            }
            count++;
        }
        curr = curr->next;
    }
    if (count == 0) snprintf(buf, BIG_BUFFER_SZ, "EMPTY");
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
    m->elapsed_at_pause = 0;
    m->is_paused = 0;
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

int match_release_after_client(Client *me) {
    if (!me) return 0;
    if (!me->match) return 0; 
    
    Match *m = me->match;

    pthread_mutex_lock(&m->lock);
    
    if (m->finished) {
        if (me == m->white) m->white = NULL;
        else if (me == m->black) m->black = NULL;

        me->match = NULL;
        if (m->refs > 0) m->refs--;
        int last = (m->refs <= 0);
        
        pthread_mutex_unlock(&m->lock);
        if (last) match_free(m);
        return 0; 
    }

    log_printf("[MATCH] Client %p (%s) disconnected. Entering grace period.\n", me, me->name);
    
    me->sock = -1; 
    me->disconnect_time = time(NULL);
    
    pthread_mutex_unlock(&m->lock);
    return 1; 
}

Client *match_reconnect(const char *name, const char *id, int new_sock) {
    pthread_mutex_lock(&room_registry_lock);
    Match *curr = global_room_list;
    while (curr) {
        pthread_mutex_lock(&curr->lock);
        if (!curr->finished) {
            Client *target = NULL;
            if (curr->white && strcmp(curr->white->name, name) == 0 && 
                strcmp(curr->white->id, id) == 0 && 
                curr->white->sock == -1) {
                target = curr->white;
            } else if (curr->black && strcmp(curr->black->name, name) == 0 && 
                       strcmp(curr->black->id, id) == 0 &&
                       curr->black->sock == -1) {
                target = curr->black;
            }

            if (target) {
                target->sock = new_sock;
                target->disconnect_time = 0;
                
                unsigned int seed = time(NULL) ^ (uintptr_t)target;
                target->seq = rand_r(&seed) & 0x1FF;
                target->last_heartbeat = time(NULL);

                log_printf("[MATCH] Client %p (%s) RECONNECTED to match %d (ID verified).\n", target, name, curr->id);
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

/* [CHANGED] Returns 1 if actually resumed, 0 if was not paused */
int match_try_resume(Match *m) {
    int resumed = 0;
    if (!m) return 0;
    pthread_mutex_lock(&m->lock);
    if (m->is_paused && m->white && m->white->sock > 0 && m->black && m->black->sock > 0) {
        m->last_move_time = time(NULL) - m->elapsed_at_pause;
        m->elapsed_at_pause = 0;
        m->is_paused = 0;
        resumed = 1;
        log_printf("[MATCH] Match %d resumed. Timer restored.\n", m->id);
    }
    pthread_mutex_unlock(&m->lock);
    return resumed;
}

int match_get_remaining_time(Match *m) {
    if (m->finished) return 0;
    if (m->is_paused) {
        int left = m->turn_timeout_seconds - m->elapsed_at_pause;
        return (left < 0) ? 0 : left;
    }
    if (m->last_move_time == 0) return m->turn_timeout_seconds;
    
    int elapsed = time(NULL) - m->last_move_time;
    int left = m->turn_timeout_seconds - elapsed;
    return (left < 0) ? 0 : left;
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
    
    int t = m->turn_timeout_seconds;
    send_fmt_with_seq(m->white, "TIME %d", t);
    send_fmt_with_seq(m->black, "TIME %d", t);
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
        if (!m->is_paused && m->last_move_time != 0 && (now - m->last_move_time) >= m->turn_timeout_seconds) {
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

        /* 2. Grace Period */
        if (m->white && m->white->sock == -1 && !m->is_paused) {
            if (now - m->white->disconnect_time > DISCONNECT_GRACE_PERIOD) {
                if (m->last_move_time > 0) {
                    m->elapsed_at_pause = now - m->last_move_time;
                    m->last_move_time = 0; 
                }
                m->is_paused = 1;
                if (m->black && m->black->sock > 0) send_fmt_with_seq(m->black, "WAIT_CONN");
            }
        }
        if (m->black && m->black->sock == -1 && !m->is_paused) {
            if (now - m->black->disconnect_time > DISCONNECT_GRACE_PERIOD) {
                if (m->last_move_time > 0) {
                    m->elapsed_at_pause = now - m->last_move_time;
                    m->last_move_time = 0; 
                }
                m->is_paused = 1;
                if (m->white && m->white->sock > 0) send_fmt_with_seq(m->white, "WAIT_CONN");
            }
        }

        /* 3. Zombie Check */
        if (m->white && m->white->sock > 0 && (now - m->white->last_heartbeat > HEARTBEAT_TIMEOUT_SECONDS)) {
            shutdown(m->white->sock, SHUT_RDWR);
            m->white->disconnect_time = now;
        }
        if (m->black && m->black->sock > 0 && (now - m->black->last_heartbeat > HEARTBEAT_TIMEOUT_SECONDS)) {
            shutdown(m->black->sock, SHUT_RDWR);
            m->black->disconnect_time = now;
        }

        /* 4. Final Disconnect */
        int w_dc = 0;
        if (m->white && m->white->sock == -1) {
             if (now - m->white->disconnect_time > DISCONNECT_TIMEOUT_SECONDS) w_dc = 1;
        }
        int b_dc = 0;
        if (m->black && m->black->sock == -1) {
             if (now - m->black->disconnect_time > DISCONNECT_TIMEOUT_SECONDS) b_dc = 1;
        }

        if (w_dc || b_dc) {
            m->finished = 1; 
            Client *winner = w_dc ? m->black : m->white; 
            if (winner && winner->sock > 0) send_line(winner->sock, "OPP_EXT");
            
            if (w_dc) { decrement_player_count(); m->refs--; }
            if (b_dc) { decrement_player_count(); m->refs--; }
            
            pthread_mutex_unlock(&m->lock);
            continue;
        }

        pthread_mutex_unlock(&m->lock);
    }
    return NULL;
}