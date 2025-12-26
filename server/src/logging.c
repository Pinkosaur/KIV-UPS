#include <ifaddrs.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h> /* Required for va_list */
#include <time.h>   /* Required for timestamps */
#include "logging.h"

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

/* printf wrapper that also appends to server.log with a timestamp */
void log_printf(const char *fmt, ...) {
    va_list args;
    
    /* 1. Standard Output (Console) */
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    /* 2. File Output */
    FILE *fp = fopen("server.log", "a");
    if (fp) {
        /* Generate Timestamp */
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
        
        /* Write Timestamp */
        fprintf(fp, "[%s] ", time_str);

        /* Write Message */
        va_start(args, fmt);
        vfprintf(fp, fmt, args);
        va_end(args);

        /* Ensure it closes to flush changes immediately */
        fclose(fp);
    }
}