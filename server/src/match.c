/* match.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "match.h"
#include "client.h"
#include "game.h"
#include "logging.h"


/* Extern globals from main.c */
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
    
    Match *curr = global_room_list;
    int count = 0;
    while (curr) {
        if (curr->black == NULL && !curr->finished) {
            char line[128];
            snprintf(line, sizeof(line), "%d:%s ", curr->id, curr->white->name);
            strcat(buf, line);
            count++;
        }
        curr = curr->next;
    }
    if (count == 0) strcpy(buf, "EMPTY");
    pthread_mutex_unlock(&room_registry_lock);
    return buf;
}

/* match.c FIX */

/* Create a new match (room) with ONE player initially. 
   refs must be 1 so that if this player leaves, the match is freed. */
Match *match_create(Client *white) {
    /* (Check max_rooms limit here if implemented) */

    Match *m = calloc(1, sizeof(Match));
    if (!m) return NULL;
    
    m->white = white;
    m->black = NULL; /* Waiting for opponent */
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

    m->last_move_time = time(NULL); 
    m->turn_timeout_seconds = TURN_TIMEOUT_SECONDS;
    
    /* FIX: Set refs to 1 (White only). Do NOT set to 2. */
    m->refs = 1; 

    /* Add to global registry (assumed implemented as register_room(m)) */
    register_room(m);

    /* Start watchdog */
    pthread_t wt;
    if (pthread_create(&wt, NULL, match_watchdog, m) == 0) {
        pthread_detach(wt);
    }

    return m;
}

/* Join an existing match */
int match_join(Match *m, Client *black) {
    pthread_mutex_lock(&m->lock);
    if (m->black != NULL || m->finished) {
        pthread_mutex_unlock(&m->lock);
        return -1; // Already full or finished
    }
    m->black = black;
    
    /* FIX: Increment refs for the new player */
    m->refs++; 
    
    pthread_mutex_unlock(&m->lock);
    return 0;
}

void match_free(Match *m) {
    if (!m) return;
    unregister_room(m);
    for (size_t i = 0; i < m->moves_count; ++i) free(m->moves[i]);
    free(m->moves);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

void match_release_after_client(Client *me) {
    if (!me || !me->match) return;
    Match *m = me->match;
    Client *other = (me == m->white) ? m->black : m->white;

    /* First: mark finished and notify opponent (under lock) */
    pthread_mutex_lock(&m->lock);
    if (!m->finished) {
        m->finished = 1;
        /* Note: We do NOT close sockets here. We just notify. */
        if (other && other->sock > 0) {
            /* Use sequence-safe send if possible, or raw send_line */
            send_line(other->sock, "OPP_EXT");
        }
    }

    /* clear our match pointer */
    me->match = NULL;

    /* decrement refs */
    m->refs--;
    int last = (m->refs <= 0);
    pthread_mutex_unlock(&m->lock);

    if (last) {
        /* Last user out frees the memory. Do NOT close sockets. */
        match_free(m);
    }
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
    /* Use send_fmt_with_seq to maintain sequence numbers */
    send_fmt_with_seq(m->white, "START %s white", m->black->name);
    send_fmt_with_seq(m->black, "START %s black", m->white->name);
}

void *match_watchdog(void *arg) {
    Match *m = (Match *)arg;
    if (!m) return NULL;

    while (1) {
        sleep(2); /* check every 2 seconds */

        pthread_mutex_lock(&m->lock);
        if (m->refs <= 0 || m->finished) {
            pthread_mutex_unlock(&m->lock);
            break;
        }
        time_t now = time(NULL);
        time_t last = m->last_move_time;
        int timeout = m->turn_timeout_seconds;
        int timed_out = 0;

        if (last != 0 && timeout > 0 && (now - last) >= timeout) {
            timed_out = 1;
        }

        if (timed_out) {
            Client *inactive = (m->turn == 0) ? m->white : m->black;
            Client *winner   = (m->turn == 0) ? m->black : m->white;

            m->finished = 1;

            if (inactive && inactive->sock > 0) send_line(inactive->sock, "TOUT");
            if (winner && winner->sock > 0) send_line(winner->sock, "OPP_TOUT");
            
            /* Do NOT close sockets. Clients will receive TOUT, send LIST, and exit game loop. */

            pthread_mutex_unlock(&m->lock);
            break;
        } else {
            pthread_mutex_unlock(&m->lock);
        }
    }
    return NULL;
}