/* client.h */
/**
 * @file client.h
 * @brief Client structure definitions and protocol constants.
 *
 * Defines the Client entity, its finite state machine states, and the 
 * text-based protocol acknowledgement codes used for communication.
 */

#ifndef CLIENT_H
#define CLIENT_H

#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include "config.h"

/* Forward declarations to avoid circular includes */
typedef struct Match Match;

/* --- Protocol Acknowledgement Codes --- */
#define MM_TOUT_ACK     "01" /**< Matchmaking timeout */
#define WAIT_ACK        "02" /**< Entered waiting state */
#define START_ACK       "03" /**< Game started */
#define ERR_ACK         "04" /**< Generic error */
#define OK_MV_ACK       "05" /**< Move accepted */
#define OPP_MV_ACK      "06" /**< Opponent moved */
#define CHK_ACK         "07" /**< Check */
#define CHKM_ACK        "08" /**< Checkmate (You lost) */
#define WIN_CHKM_ACK    "09" /**< Checkmate (You won) */
#define DRW_OFF_ACK_SC  "10" /**< Draw offered (Server->Client) */
#define DRW_DCD_ACK     "11" /**< Draw declined */
#define DRW_ACD_ACK     "12" /**< Draw agreed */
#define RES_ACK_SC      "13" /**< Resignation confirmed */
#define OPP_RES_ACK     "14" /**< Opponent resigned */
#define TOUT_ACK        "15" /**< Timeout (You lost) */
#define OPP_TOUT_ACK    "16" /**< Opponent timeout (You won) */
#define OPP_EXT_ACK     "17" /**< Opponent disconnected/exited */
#define HELLO_ACK       "18" /**< Handshake successful */
#define MV_ACK          "19" /**< Move received (internal use) */
#define DRW_OFF_ACK_CS  "20" /**< Draw offer received (Client->Server) */
#define DRW_DEC_ACK     "21" /**< Draw decline received */
#define DRW_ACC_ACK     "22" /**< Draw accept received */
#define RES_ACK_CS      "23" /**< Resignation received */
#define SM_ACK          "25" /**< Stalemate */
#define RESUME_ACK      "26" /**< Game resumed */

/**
 * @brief Enumeration of possible client states in the Finite State Machine.
 */
typedef enum {
    STATE_HANDSHAKE,    /**< Initial connection, awaiting HELLO */
    STATE_LOBBY,        /**< Authenticated, browsing rooms */
    STATE_WAITING,      /**< Created a room, waiting for opponent */
    STATE_GAME,         /**< Actively playing a match */
    STATE_DISCONNECTED  /**< Connection closed, pending cleanup */
} ClientState;

/**
 * @brief Represents a connected user session.
 */
typedef struct Client {
    int sock;                       /**< Active TCP socket file descriptor */
    char name[NAME_LEN];            /**< Display name */
    char id[ID_LEN];                /**< Unique persistent session identifier */
    int color;                      /**< 0 for White, 1 for Black */
    int paired;                     /**< Flag indicating if opponent has joined */
    Match *match;                   /**< Pointer to current match (if any) */
    char client_addr[ADDR_LEN];     /**< String representation of client IP:Port */
    
    /* State Management */
    ClientState state;              /**< Current FSM state */
    int seq;                        /**< Outbound sequence number for protocol reliability */
    int error_count;                /**< Counter for protocol violations */
    int is_counted;                 /**< Flag: 1 if this client is counted in global stats */
    
    /* Concurrency & Timing */
    pthread_mutex_t lock;           /**< Mutex protecting client state */
    time_t disconnect_time;         /**< Timestamp when socket was lost (for grace period) */
    time_t last_heartbeat;          /**< Timestamp of last received data */
    
    /* Registry Linkage */
    struct Client *next_global;     /**< Next pointer for the global client registry list */
} Client;

/* --- Function Prototypes --- */

/**
 * @brief Entry point for the client handling thread.
 * @param arg Pointer to the Client structure.
 */
void *client_worker(void *arg);

/**
 * @brief Sends raw data to the client socket.
 */
void send_raw(int sock, const char *msg);

/**
 * @brief Sends a line of text terminated by a newline.
 */
void send_line(int sock, const char *msg);

/**
 * @brief Sends an error message using the protocol format.
 */
void send_error(Client *c, const char *reason);

/**
 * @brief Sends a formatted message appended with a sequence number.
 */
void send_fmt_with_seq(Client *c, const char *fmt, ...);

/**
 * @brief Sends a short acknowledgement packet.
 */
void send_short_ack(Client *c, const char *ack_code, int recv_seq);

/* Utility Functions */
void trim_crlf(char *s);
int parse_seq_number(const char *line);
void strip_trailing_seq(char *buf);
const char *ack_code_for_received(const char *cmd);
void reject_connection(int sock);

/* Global Limit Management (wrappers around locks) */
int get_online_players(void);
void increment_player_count(void);
void decrement_player_count(void);

#endif /* CLIENT_H */