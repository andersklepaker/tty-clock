#ifndef COPILOT_H_INCLUDED
#define COPILOT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define COPILOT_FORECAST_ROWS 6
#define COPILOT_ROWS (4 + COPILOT_FORECAST_ROWS)
#define COPILOT_WIDTH 54
#define COPILOT_TODAY_GRAPH_ROW 4

typedef struct {
     bool valid, stale, unlimited, credits, has_quota;
     double percent_used, used, total;
} copilot_usage;

extern copilot_usage copilot;
void copilot_tick(bool enabled, unsigned int interval);
void copilot_cleanup(void);
void copilot_lines(char *bar, size_t bar_size, char *detail, size_t detail_size,
                   int width);
/* Fill COPILOT_FORECAST_ROWS additional display rows. */
void copilot_forecast_lines(time_t now, char (*lines)[256], int width);
/* Fill quota, today and forecast groups, including blank separator rows. */
void copilot_display_lines(time_t now, char (*lines)[256], int width);

#endif
