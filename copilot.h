#ifndef COPILOT_H_INCLUDED
#define COPILOT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>

typedef struct {
     bool valid, stale, unlimited, credits, has_quota;
     double percent_used, used, total;
} copilot_usage;

extern copilot_usage copilot;
void copilot_tick(bool enabled, unsigned int interval);
void copilot_cleanup(void);
void copilot_lines(char *bar, size_t bar_size, char *detail, size_t detail_size,
                   int width);

#endif
