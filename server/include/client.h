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

/* Forward declaration */
typedef struct Match Match;

/* --- Protocol Messages (Commands & Payloads) --- */
#define WELCOME             "WELCOME"         /**< Server greeting message */
#define HELLO               "HELLO "          /**< Client handshake: "HELLO <Name> <ID>" */
#define PLAYER_LIMIT_REACHED "FULL\n"         /**< Rejection message when server is full */
#define ENTER_LOBBY         "LOBBY"           /**< Client request to enter lobby state */
#define ROOM_LIST_REQUEST   "LIST"            /**< Client request for list of active rooms */
#define ROOM_LIST_ANSWER    "ROOMLIST %s"     /**< Server response containing room list */
#define CREATE_ROOM         "NEW"             /**< Client request to create a new room */
#define WAIT                "WAITING Room %d" /**< Server notification: Waiting for opponent in Room X */
#define JOIN_ROOM           "JOIN "           /**< Client request to join room: "JOIN <RoomID>" */
#define START_AS_WHITE      "START %s white"  /**< Notification: Game start, playing White */
#define START_AS_BLACK      "START %s black"  /**< Notification: Game start, playing Black */
#define YOU_TIMED_OUT       "TOUT"            /**< Notification: You ran out of time */
#define OPPONENT_TIMED_OUT  "OPP_TOUT"        /**< Notification: Opponent ran out of time */
#define WAIT_FOR_RECONNECT  "WAIT_CONN"       /**< Notification: Opponent disconnected, waiting... */
#define RESUME_MATCH        "RESUME %s %s"    /**< Notification: Match resumed after reconnect */
#define OPPONENT_RETURNED   "OPP_RESUME %s %s"/**< Notification: Opponent has reconnected */
#define MATCH_HISTORY       "HISTORY %s"      /**< Send move history to reconnecting client */
#define MOVE_COMMAND        "MV"              /**< Client move command: "MV <move>" */
#define OPPONENT_MOVE       "OPP_MV %s"       /**< Notification: Opponent made a move */
#define ACCEPT_MOVE         "OK_MV"           /**< Confirmation: Your move was valid and accepted */
#define TURN_TIMER_STATE    "TIME %d"         /**< Update: Remaining time for current turn */
#define IN_CHECK            "CHK"             /**< Notification: You are in check */
#define WON_BY_CHECKMATE    "WIN_CHKM"        /**< Notification: You won by checkmate */
#define LOST_BY_CHECKMATE   "CHKM"            /**< Notification: You lost by checkmate */
#define STALEMATE           "SM"              /**< Notification: Game ended in stalemate */
#define RESIGN              "RES"             /**< Client command: Resign game */
#define YOU_RESIGNED        "RES"             /**< Confirmation: You resigned */
#define OPPONENT_RESIGNED   "OPP_RES"         /**< Notification: Opponent resigned */
#define DRAW_OFFER          "DRW_OFF"         /**< Client command: Offer draw */
#define ACCEPT_DRAW         "DRW_ACC"         /**< Client command: Accept draw offer */
#define DRAW_ACCEPTED       "DRW_ACD"         /**< Notification: Draw offer accepted */
#define DECLINE_DRAW        "DRW_DEC"         /**< Client command: Decline draw offer */
#define DRAW_DECLINED       "DRW_DCD"         /**< Notification: Draw offer declined */
#define EXIT                "EXT"             /**< Client command: Exit current context */
#define OPPONENT_QUIT       "OPP_EXT"         /**< Notification: Opponent left the game */
#define OPPONENT_KICKED_OUT "OPP_KICK"        /**< Notification: Opponent kicked for protocol violation */
#define PING                "PING"            /**< Heartbeat request */
#define PING_RESPONSE       "PNG"             /**< Heartbeat response */

/* --- Protocol Acknowledgement Codes --- */
/* Server -> Client Confirmations */
#define MATCHMAKING_TOUT_ACK    "01" /**< Matchmaking timeout occurred */
#define WAIT_ACK                "02" /**< Entered waiting state (Room created) */
#define START_ACK               "03" /**< Game started successfully */
#define ERR_ACK                 "04" /**< Generic error occurred */
#define ACCEPT_MOVE_ACK         "05" /**< Move accepted (Alternative to OK_MV) */
#define OPPONENT_MOVE_ACK       "06" /**< Opponent move broadcast */
#define CHECK_ACK               "07" /**< Check condition notification */
#define LOST_BY_CHECKMATE_ACK   "08" /**< Checkmate (You lost) notification */
#define WIN_BY_CHEKMATE_ACK     "09" /**< Checkmate (You won) notification */
#define DRAW_OFFER_ACK_SC       "10" /**< Draw offered (Server->Client notification) */
#define DRAW_DECLINED_ACK       "11" /**< Draw declined notification */
#define DRAW_ACCEPTED_ACK       "12" /**< Draw agreed notification */
#define RESIGN_ACK_SC           "13" /**< Resignation confirmed (Server->Client) */
#define OPPONENT_RESIGNED_ACK   "14" /**< Opponent resigned notification */
#define TOU_TIMED_OUT_ACK       "15" /**< Timeout (You lost) notification */
#define OPPONENT_TIMED_OUT_ACK  "16" /**< Opponent timeout (You won) notification */
#define OPPONENT_QUIT_ACK       "17" /**< Opponent disconnected/exited notification */
#define HELLO_ACK               "18" /**< Handshake successful */

/* Client -> Server Receipt Confirmations */
#define MOVE_COMMAND_ACK        "19" /**< ACK: Move command received */
#define DRAW_OFFER_ACK_CS       "20" /**< ACK: Draw offer received */
#define DECLINE_DRAW_ACK        "21" /**< ACK: Draw decline received */
#define ACCEPT_DRAW_ACK         "22" /**< ACK: Draw accept received */
#define RESIGN_ACK_CS           "23" /**< ACK: Resignation received */
#define STALEMATE_ACK           "25" /**< ACK: Stalemate condition */
#define RESUME_ACK              "26" /**< ACK: Game resumed */

/* State Transition ACKs */
#define LOBBY_ACK               "27" /**< ACK: Entered Lobby */
#define NEW_ROOM_ACK            "28" /**< ACK: Room creation request received */
#define JOIN_REQ_ACK            "29" /**< ACK: Join room request received */
#define LIST_REQ_ACK            "30" /**< ACK: Room list request received */
#define EXIT_ACK                "31" /**< ACK: Exit/Disconnect request received */

#define GENERIC_ACK             "99" /**< Fallback ACK for undefined commands */

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
 * @brief Formats a protocol message and sends it to the client.
 */
void send_protocol_msg(Client *c, const char *fmt, ...);

/**
 * @brief Sends a short acknowledgement packet.
 */
void send_short_ack(Client *c, const char *ack_code);

/* Utility Functions */
void trim_crlf(char *s);
const char *ack_code_for_received(const char *cmd);
void reject_connection(int sock);

/* Global Limit Management (wrappers around locks) */
int get_online_players(void);
void increment_player_count(void);
void decrement_player_count(void);

#endif /* CLIENT_H */