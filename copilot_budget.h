#ifndef COPILOT_BUDGET_H_INCLUDED
#define COPILOT_BUDGET_H_INCLUDED

#include <stdbool.h>
#include <time.h>

typedef struct {
     bool valid, partial, storage_error, has_rate, monthly_rate, holds;
     double today_used, daily_budget, remaining, daily_rate, safe_rate, needed_until_reset;
     time_t reset_at, empty_at, observed_since;
} copilot_forecast;

void copilot_budget_sample(time_t now, double used, double total, bool credits);
void copilot_budget_get(time_t now, copilot_forecast *forecast);

#endif
