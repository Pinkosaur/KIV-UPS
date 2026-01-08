/* logging.h */
/**
 * @file logging.h
 * @brief Logging subsystem interface.
 *
 * Provides thread-safe logging capabilities and network interface discovery utilities.
 */

#ifndef LOGGING_H
#define LOGGING_H

#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/**
 * @brief Initializes the background logging thread and opens the log file.
 */
void init_logging(void);

/**
 * @brief Flushes logs, stops the thread, and closes the file.
 */
void close_logging(void);

/**
 * @brief Prints available local IPv4 interfaces to the log.
 */
void list_local_interfaces(void);

/**
 * @brief Retrieves the interface name associated with a specific IP address.
 * @param inaddr The IP address to search for.
 * @param ifname_out Buffer to store the interface name.
 * @param ifname_out_sz Size of the output buffer.
 * @return 1 if found, 0 otherwise.
 */
int get_interface_name_for_addr(struct in_addr inaddr, char *ifname_out, size_t ifname_out_sz);

/**
 * @brief Thread-safe formatted logging function.
 * Adds timestamp and writes to both stdout and the log file.
 * @param fmt Printf-style format string.
 * @param ... Arguments.
 */
void log_printf(const char *fmt, ...);

#endif /* LOGGING_H */