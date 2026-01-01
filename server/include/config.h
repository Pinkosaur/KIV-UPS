/* config.h */
#ifndef CONFIG_H
#define CONFIG_H

#define DEFAULT_PORT 10001
#define DEFAULT_IP "0.0.0.0"

/* Buffers & Limits */
#define BUFFER_SZ 1024
#define LINEBUF_SZ 256
#define BIG_BUFFER_SZ 4096
#define NAME_LEN 64
#define ADDR_LEN 64
#define RECV_SUFFIX_LEN 16

/* Logic Constants */
#define MAX_ERRORS 3
#define TURN_TIMEOUT_SECONDS 180
#define DISCONNECT_TIMEOUT_SECONDS 60
#define HEARTBEAT_TIMEOUT_SECONDS 15
#define RECONNECT_WINDOW 60

#endif /* CONFIG_H */