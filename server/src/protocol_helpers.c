/* protocol_helpers.c */
#include "protocol_helpers.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <stdlib.h>

#define MODULO 512
#define SUFFIX_FMT "/%03d"
#define MAXLINE 1024

/* send a line like "<msg>/<suffix>\n" - returns 0 on success, -1 on error */
int send_with_counter(int sock, const char *msg_without_suffix, uint16_t counter) {
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "%s/%03d\n", msg_without_suffix, counter % MODULO);
    if (n < 0 || n >= (int)sizeof(buf)) return -1;
    ssize_t w = send(sock, buf, n, 0);
    if (w != n) {
        /* partial or error */
        return -1;
    }
    return 0;
}

/* parse trailing /NNN suffix from a line (without newline).
   returns 1 on success and writes integer to suffix_out, 0 on failure */
int parse_suffix(const char *line, int *suffix_out) {
    size_t L = strlen(line);
    if (L < 4) return 0; /* must at least have "/000" */
    /* find last '/' */
    const char *p = strrchr(line, '/');
    if (!p) return 0;
    ++p; /* points to first digit */
    /* ensure exactly 3 digits (but allow 1..3 for robustness) */
    int v = 0;
    int digits = 0;
    while (*p && digits < 4) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            digits++;
            p++;
        } else break;
    }
    if (digits < 1 || digits > 3) return 0;
    if (suffix_out) *suffix_out = v % MODULO;
    return 1;
}

/* wait for a line from sock and validate that its suffix equals expected_suffix.
   timeout_ms: milliseconds to wait.
   on success copies the received full line (without \n) to out_line if out_line != NULL and returns 1.
   returns 0 on timeout, -1 on socket error */
int wait_for_ack(int sock, int expected_suffix, int timeout_ms, char *out_line, size_t out_sz) {
    fd_set rfds;
    struct timeval tv;
    char buf[MAXLINE];
    size_t off = 0;

    int rv;
    int total_waited = 0;
    int poll_interval_ms = 100; /* small chunk to be able to read partial lines */
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        tv.tv_sec = poll_interval_ms / 1000;
        tv.tv_usec = (poll_interval_ms % 1000) * 1000;
        rv = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rv == 0) {
            total_waited += poll_interval_ms;
            if (total_waited >= timeout_ms) return 0; /* timeout */
            continue;
        }
        /* data available */
        ssize_t r = recv(sock, buf + off, sizeof(buf) - off - 1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1; /* socket error */
        }
        if (r == 0) {
            /* peer closed connection */
            return -1;
        }
        off += (size_t)r;
        buf[off] = '\0';
        /* look for newline */
        char *nl;
        while ((nl = strchr(buf, '\n')) != NULL) {
            *nl = '\0'; /* terminate the line */
            /* Now check suffix */
            int suffix;
            if (parse_suffix(buf, &suffix)) {
                if (suffix == (expected_suffix % MODULO)) {
                    if (out_line && out_sz > 0) {
                        strncpy(out_line, buf, out_sz-1);
                        out_line[out_sz-1] = '\0';
                    }
                    /* consume remainder after this newline by shifting buffer left */
                    size_t remaining = off - ((nl - buf) + 1);
                    if (remaining > 0) memmove(buf, nl + 1, remaining);
                    off = remaining;
                    buf[off] = '\0';
                    return 1; /* success */
                }
            }
            /* not the ack we want — optionally we can ignore or handle this line here.
               For now, drop it and continue waiting. */
            /* drop processed line and continue loop */
            size_t remaining = off - ((nl - buf) + 1);
            if (remaining > 0) memmove(buf, nl + 1, remaining);
            off = remaining;
            buf[off] = '\0';
        }
        /* no newline yet; keep receiving until timeout */
        total_waited += poll_interval_ms;
        if (total_waited >= timeout_ms) return 0;
    }
}
