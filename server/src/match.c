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

/* create a new match for two clients (waiting becomes white, newcomer black) */
Match *match_create(Client *white, Client *black) {
    Match *m = calloc(1, sizeof(Match));
    if (!m) return NULL;
    m->white = white;
    m->black = black;
    m->turn = 0; /* white to move first */
    pthread_mutex_init(&m->lock, NULL);
    m->moves = NULL;
    m->moves_count = 0;
    m->moves_cap = 0;
    m->finished = 0;   /* init finished flag */
    m->draw_offered_by = -1;

    /* initial castling rights: both sides full rights */
    m->w_can_kingside = 1;
    m->w_can_queenside = 1;
    m->b_can_kingside = 1;
    m->b_can_queenside = 1;

    /* no en-passant target initially */
    m->ep_r = -1;
    m->ep_c = -1;

    init_board(&m->state); /* initialize game state */

    /* initialization for inactivity detection */
    m->last_move_time = time(NULL); /* turn (white) starts now */
    m->turn_timeout_seconds = TURN_TIMEOUT_SECONDS;

    /* initialize client cleanup refcount: two client threads (white & black) */
    m->refs = 2;

    /* start a detached watchdog thread for this match */
    pthread_t wt;
    if (pthread_create(&wt, NULL, match_watchdog, m) == 0) {
        pthread_detach(wt);
    }

    return m;
}


/* free match and move history */
void match_free(Match *m) {
    if (!m) return;
    for (size_t i = 0; i < m->moves_count; ++i) free(m->moves[i]);
    free(m->moves);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

/* Called by a client thread during its cleanup path.
   Ensures:
   - notify / wake opponent so it will exit recv()
   - decrement match->refs (under lock)
   - free match only when both client threads have reached cleanup (refs==0)
*/
void match_release_after_client(Client *me) {
    if (!me || !me->match) return;
    Match *m = me->match;
    Client *other = (me == m->white) ? m->black : m->white;

    /* First: mark finished and notify opponent (under lock) and wake recv() */
    pthread_mutex_lock(&m->lock);
    if (!m->finished) {
        m->finished = 1;
        if (other && other->sock > 0) {
            send_line(other->sock, "OPPONENT_QUIT");
        }
    }

    /* Wake the other thread (if blocked in recv) so it can run its cleanup */
    if (other && other->sock > 0) {
        /* shutdown wakes blocked recv() on the other side */
        shutdown(other->sock, SHUT_RDWR);
    }

    /* clear our match pointer (we do not free the Client here) */
    me->match = NULL;

    /* decrement refs under lock, remember whether we're the last */
    m->refs--;
    int last = (m->refs <= 0);
    pthread_mutex_unlock(&m->lock);

    if (last) {
        /* Both client threads reached cleanup. At this point both clients
           should have closed/marked their sockets; but for safety ensure sockets
           are shut/closed before freeing the match. */
        if (m->white && m->white->sock > 0) {
            shutdown(m->white->sock, SHUT_RDWR);
            close(m->white->sock);
            m->white->sock = -1;
        }
        if (m->black && m->black->sock > 0) {
            shutdown(m->black->sock, SHUT_RDWR);
            close(m->black->sock);
            m->black->sock = -1;
        }

        /* finally free match resources */
        match_free(m);
    }
}


/* append move string to match history (caller should lock) */
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


/* send START messages to both clients indicating opponent and color */
void notify_start(Match *m) {
    char buf[128];
    if (!m) return;
    /* to white: opponent name, your color */
    snprintf(buf, sizeof(buf), "START %s white", m->black->name);
    send_line(m->white->sock, buf);
    /* to black: opponent name, your color */
    snprintf(buf, sizeof(buf), "START %s black", m->white->name);
    send_line(m->black->sock, buf);
}


/* watchdog thread for a match: ends match if current player exceeds inactivity timeout */
void *match_watchdog(void *arg) {
    Match *m = (Match *)arg;
    if (!m) return NULL;

    while (1) {
        sleep(2); /* check every 2 seconds */

        pthread_mutex_lock(&m->lock);
        if (m->refs <= 0) {
            pthread_mutex_unlock(&m->lock);
            break;
        }
        if (m->finished) {
            pthread_mutex_unlock(&m->lock);
            break;
        }
        time_t now = time(NULL);
        time_t last = m->last_move_time;
        int timeout = m->turn_timeout_seconds;
        int timed_out = 0;

        if (last != 0 && timeout > 0 && (now - last) >= timeout) {
            /* the player whose turn it is failed to act in time */
            timed_out = 1;
        }

        if (timed_out) {
            /* Determine inactive (timed-out) and winner */
            Client *inactive = (m->turn == 0) ? m->white : m->black;
            Client *winner   = (m->turn == 0) ? m->black : m->white;

            /* mark finished */
            m->finished = 1;

            /* send messages if sockets exist and close them */
            if (inactive && inactive->sock > 0) {
                send_line(inactive->sock, "TIMEOUT"); /* you timed out */
                shutdown(inactive->sock, SHUT_RDWR);
                close(inactive->sock);
                inactive->sock = -1;
            }
            if (winner && winner->sock > 0) {
                send_line(winner->sock, "OPPONENT_TIMEOUT"); /* opponent timed out -> you win */
                shutdown(winner->sock, SHUT_RDWR);
                close(winner->sock);
                winner->sock = -1;
            }

            pthread_mutex_unlock(&m->lock);
            break;
        } else {
            pthread_mutex_unlock(&m->lock);
        }
    }

    return NULL;
}