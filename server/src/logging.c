/* src/logging.c */
/**
 * @file logging.c
 * @brief Thread-safe logging subsystem.
 *
 * Implements a background logging thread that consumes messages from a thread-safe queue.
 * This ensures that I/O operations (file writing/console output) do not block the
 * critical game logic threads.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include "logging.h"

/**
 * Linked list node for buffered log messages.
 */
typedef struct LogNode {
    char *message;
    struct LogNode *next;
} LogNode;

static LogNode *head = NULL;
static LogNode *tail = NULL;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_t logging_tid;
static volatile int logging_running = 0;
static FILE *log_fp = NULL;

/**
 * @brief Background thread function that processes the log queue.
 * Writes messages to the log file and standard output.
 */
static void *logger_thread_func(void *arg) {
    (void)arg;
    LogNode *batch_head = NULL;

    while (1) {
        pthread_mutex_lock(&queue_lock);
        while (head == NULL && logging_running) {
            pthread_cond_wait(&queue_cond, &queue_lock);
        }
        if (!logging_running && head == NULL) {
            pthread_mutex_unlock(&queue_lock);
            break;
        }
        // Extract the entire current queue to process outside the lock
        batch_head = head;
        head = NULL;
        tail = NULL;
        pthread_mutex_unlock(&queue_lock);

        if (batch_head) {
            LogNode *current = batch_head;
            while (current) {
                if (log_fp) fprintf(log_fp, "%s", current->message);
                printf("%s", current->message);
                
                LogNode *temp = current;
                current = current->next;
                free(temp->message);
                free(temp);
            }
            if (log_fp) fflush(log_fp);
            fflush(stdout); 
        }
    }
    return NULL;
}

/**
 * @brief Initializes the logging subsystem.
 * Opens the log file and starts the background logger thread.
 */
void init_logging(void) {
    if (logging_running) return;
    log_fp = fopen("server.log", "a");
    logging_running = 1;
    pthread_create(&logging_tid, NULL, logger_thread_func, NULL);
}

/**
 * @brief Shuts down the logging subsystem.
 * Flushes pending logs, stops the thread, and closes the file.
 */
void close_logging(void) {
    pthread_mutex_lock(&queue_lock);
    logging_running = 0;
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_lock);
    if (logging_tid) pthread_join(logging_tid, NULL);
    if (log_fp) { fclose(log_fp); log_fp = NULL; }
}

/**
 * @brief Enqueues a formatted log message.
 * Thread-safe and non-blocking (unless memory allocation fails).
 * @param fmt Printf-style format string.
 * @param ... Arguments for the format string.
 */
void log_printf(const char *fmt, ...) {
    if (!logging_running) return;

    /* 1. Format Timestamp and Message */
    char timebuf[64];
    time_t now = time(NULL);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    va_list args;
    va_start(args, fmt);
    int len_msg = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (len_msg < 0) return;

    size_t total_len = strlen(timebuf) + 3 + len_msg + 1;
    char *entry_str = malloc(total_len);
    if (!entry_str) return;

    sprintf(entry_str, "[%s] ", timebuf);
    va_start(args, fmt);
    vsnprintf(entry_str + strlen(timebuf) + 3, len_msg + 1, fmt, args);
    va_end(args);

    /* 2. Push to Queue */
    LogNode *node = malloc(sizeof(LogNode));
    if (node) {
        node->message = entry_str;
        node->next = NULL;
        pthread_mutex_lock(&queue_lock);
        if (tail) { tail->next = node; tail = node; }
        else { head = node; tail = node; }
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_lock);
    } else {
        free(entry_str);
    }
}

/**
 * @brief Lists local network interfaces and IPs to the log.
 * Useful for determining the server's binding address.
 */
void list_local_interfaces(void) {
    struct ifaddrs *ifaddr, *ifa;
    char addrbuf[INET_ADDRSTRLEN];
    if (getifaddrs(&ifaddr) == -1) return;
    log_printf("Local network interfaces (IPv4):\n");
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, addrbuf, sizeof(addrbuf));
        log_printf("  %s: %s\n", ifa->ifa_name, addrbuf);
    }
    freeifaddrs(ifaddr);
}

/**
 * @brief Helper to find the interface name for a given IP address.
 * @return 1 if found, 0 otherwise.
 */
int get_interface_name_for_addr(struct in_addr inaddr, char *ifname_out, size_t ifname_out_sz) {
    struct ifaddrs *ifaddr, *ifa;
    int found = 0;
    if (getifaddrs(&ifaddr) == -1) return 0;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        if (sa->sin_addr.s_addr == inaddr.s_addr) {
            strncpy(ifname_out, ifa->ifa_name, ifname_out_sz-1);
            ifname_out[ifname_out_sz-1] = '\0';
            found = 1; break;
        }
    }
    freeifaddrs(ifaddr);
    return found;
}