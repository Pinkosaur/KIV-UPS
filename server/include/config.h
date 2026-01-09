/**
 * @file config.h
 * @brief Global configuration constants and limits.
 *
 * This file contains all tunable parameters, buffer sizes, and timeout definitions
 * used throughout the server. It serves as the single source of truth for
 * array dimensions and logic thresholds.
 */

#ifndef CONFIG_H
#define CONFIG_H

/* Network Defaults */
#define DEFAULT_PORT 10001
#define DEFAULT_IP "0.0.0.0"

/* Buffers & Data Limits */
#define BUFFER_SZ 1024          /**< Size of the raw socket read buffer */
#define LINEBUF_SZ 256          /**< Maximum length of a single protocol line */
#define BIG_BUFFER_SZ 4096      /**< Size for large payloads (e.g., history, room lists) */
#define NAME_LEN 64             /**< Maximum length of a client name */
#define ADDR_LEN 64             /**< Maximum length of a stringified IP address */
#define ID_LEN 32               /**< Length of the unique session ID string */

/* Application Logic Constants */
#define MAX_ERRORS 3            /**< Disconnect client after this many protocol violations */
#define TURN_TIMEOUT_SECONDS 180    /**< Max time allowed for a player to make a move */
#define DISCONNECT_TIMEOUT_SECONDS 60 /**< Time before a disconnected session is destroyed */
#define HEARTBEAT_TIMEOUT_SECONDS 15  /**< Time without data before assuming a zombie connection */
#define RECONNECT_WINDOW 60     /**< Window allowing reconnection (often same as disconnect timeout) */
#define DISCONNECT_GRACE_PERIOD 3 /**< Seconds to wait before notifying opponent of disconnect */

#endif /* CONFIG_H */