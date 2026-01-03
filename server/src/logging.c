/* logging.c */
#include <ifaddrs.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h> 
#include <time.h>   
#include "logging.h"

/* Keep file open statically to avoid opening/closing on every write */
static FILE *log_fp = NULL;

/* Initialize logging: open file and disable buffering for crash safety */
void init_logging(void) {
    if (!log_fp) {
        log_fp = fopen("server.log", "a");
        /* Disable buffering (_IONBF) so logs are flushed to the OS immediately.
           This ensures logs are preserved even if the application crashes/segfaults. */
        if (log_fp) setvbuf(log_fp, NULL, _IONBF, 0); 
    }
}

/* Optional clean up (called by OS on exit anyway, but good practice) */
void close_logging(void) {
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

/* Print all IPv4 interfaces and addresses on startup */
void list_local_interfaces(void) {
    struct ifaddrs *ifaddr, *ifa;
    char addrbuf[INET_ADDRSTRLEN];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return;
    }

    log_printf("Local network interfaces (IPv4):\n");
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            if (inet_ntop(AF_INET, &sa->sin_addr, addrbuf, sizeof(addrbuf))) {
                log_printf("  %s: %s\n", ifa->ifa_name, addrbuf);
            }
        }
    }
    freeifaddrs(ifaddr);
}

/* Given IPv4 address (in_addr), find interface name that has this address (returns 1 on success) */
int get_interface_name_for_addr(struct in_addr inaddr, char *ifname_out, size_t ifname_out_sz) {
    struct ifaddrs *ifaddr, *ifa;
    int found = 0;

    if (getifaddrs(&ifaddr) == -1) return 0;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        if (sa->sin_addr.s_addr == inaddr.s_addr) {
            strncpy(ifname_out, ifa->ifa_name, ifname_out_sz-1);
            ifname_out[ifname_out_sz-1] = '\0';
            found = 1;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found;
}

/* * printf wrapper that also appends to server.log with a timestamp.
 * Uses the static log_fp for performance.
 */
void log_printf(const char *fmt, ...) {
    va_list args;
    
    /* 1. Standard Output (Console) */
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    /* 2. File Output */
    /* Lazy initialization ensures it works even if init_logging wasn't called manually */
    if (!log_fp) init_logging();
    
    if (log_fp) {
        /* Generate Timestamp */
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);
        
        /* Write Timestamp */
        fprintf(log_fp, "[%s] ", timebuf);
        
        /* Write Message */
        va_start(args, fmt);
        vfprintf(log_fp, fmt, args);
        va_end(args);
        
        /* No explicit fflush/fclose needed here due to _IONBF */
    }
}