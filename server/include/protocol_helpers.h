#ifndef PROTOCOL_HELPERS_H
#define PROTOCOL_HELPERS_H

#include <stdint.h>
#include <stdlib.h>

int send_with_counter(int sock, const char *msg_without_suffix, uint16_t counter);
int parse_suffix(const char *line, int *suffix_out);
int wait_for_ack(int sock, int expected_suffix, int timeout_ms, char *out_line, size_t out_sz);

#endif
