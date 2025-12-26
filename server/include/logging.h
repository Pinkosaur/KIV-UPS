#ifndef LOGGING_H
#define LOGGING_H

#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void list_local_interfaces(void);
int get_interface_name_for_addr(struct in_addr inaddr, char *ifname_out, size_t ifname_out_sz);
void log_printf(const char *fmt, ...);

#endif