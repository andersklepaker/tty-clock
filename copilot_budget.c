#include "copilot_budget.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define DAY 86400
#define HISTORY_DAYS 9

typedef struct {
     time_t day, first_at, last_at;
     double baseline, first_used, last_used;
     int partial;
} daily_sample;

typedef struct {
     time_t month;
     double total;
     int credits, count;
     daily_sample days[HISTORY_DAYS];
} usage_history;

static usage_history history;
static bool storage_error;

static time_t
month_start(time_t now, int offset)
{
     struct tm utc;
     gmtime_r(&now, &utc);
     utc.tm_mday = 1;
     utc.tm_hour = utc.tm_min = utc.tm_sec = 0;
     utc.tm_mon += offset;
     return timegm(&utc);
}

/* Create only the configured state directory and its missing parents. */
static bool
state_directory(char *path, size_t size)
{
     const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
     int n;
     if (state && state[0] == '/')
          n = snprintf(path, size, "%s/tty-clock", state);
     else if (home && home[0] == '/')
          n = snprintf(path, size, "%s/.local/state/tty-clock", home);
     else
          return false;
     if (n < 0 || (size_t)n >= size) return false;
     for (char *p = path + 1; ; ++p) {
          if (*p != '/' && *p != '\0') continue;
          char end = *p;
          *p = '\0';
          int result = mkdir(path, 0700);
          *p = end;
          if (result < 0 && errno != EEXIST) return false;
          if (!end) break;
     }
     return true;
}

static bool
load_history(const char *path, usage_history *loaded)
{
     FILE *file = fopen(path, "r");
     if (!file) return false;
     struct stat info;
     if (fstat(fileno(file), &info) < 0 || !S_ISREG(info.st_mode) || info.st_size > 4096) {
          fclose(file);
          return false;
     }
     usage_history data = {0};
     long long month = 0;
     bool valid = fscanf(file, "tty-clock-v1 %lld %lf %d %d", &month,
                         &data.total, &data.credits, &data.count) == 4 &&
                  month > 0 && isfinite(data.total) && data.total > 0 &&
                  (data.credits == 0 || data.credits == 1) &&
                  data.count > 0 && data.count <= HISTORY_DAYS;
     data.month = (time_t)month;
     for (int i = 0; valid && i < data.count; ++i) {
          daily_sample *s = &data.days[i];
          long long day, first, last;
          valid = fscanf(file, "%lld %lld %lld %lf %lf %lf %d", &day,
                         &first, &last, &s->baseline, &s->first_used,
                         &s->last_used, &s->partial) == 7;
          if (!valid) break;
          s->day = (time_t)day;
          s->first_at = (time_t)first;
          s->last_at = (time_t)last;
          valid = day >= month && day % DAY == 0 && first >= day &&
                  last >= first && last < day + DAY &&
                  isfinite(s->baseline) && isfinite(s->first_used) &&
                  isfinite(s->last_used) && s->baseline >= 0 &&
                  s->first_used >= s->baseline && s->last_used >= s->first_used &&
                  s->last_used <= data.total &&
                  (s->partial == 0 || s->partial == 1) &&
                  (!i || (s->day > data.days[i - 1].day &&
                          s->baseline >= data.days[i - 1].last_used));
     }
     int c;
     while (valid && (c = fgetc(file)) != EOF)
          if (c != ' ' && c != '\n' && c != '\t' && c != '\r') valid = false;
     if (ferror(file)) valid = false;
     fclose(file);
     if (valid) *loaded = data;
     return valid;
}

static bool
save_history(const char *path)
{
     char temporary[PATH_MAX];
     int n = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
     if (n < 0 || (size_t)n >= sizeof(temporary)) return false;
     int fd = mkstemp(temporary);
     if (fd < 0) return false;
     FILE *file = fdopen(fd, "w");
     if (!file) {
          close(fd);
          unlink(temporary);
          return false;
     }
     bool ok = fprintf(file, "tty-clock-v1 %lld %.17g %d %d\n",
                       (long long)history.month, history.total,
                       history.credits, history.count) >= 0;
     for (int i = 0; i < history.count; ++i) {
          daily_sample *s = &history.days[i];
          if (fprintf(file, "%lld %lld %lld %.17g %.17g %.17g %d\n",
                      (long long)s->day, (long long)s->first_at,
                      (long long)s->last_at, s->baseline, s->first_used,
                      s->last_used, s->partial) < 0) ok = false;
     }
     if (fclose(file) != 0) ok = false;
     if (ok && rename(temporary, path) == 0) return true;
     unlink(temporary);
     return false;
}

void
copilot_budget_sample(time_t now, double used, double total, bool credits)
{
     if (now <= 0 || !isfinite(used) || !isfinite(total) ||
         total <= 0 || used < 0 || used > total) return;

     char directory[PATH_MAX], path[PATH_MAX], lock_path[PATH_MAX];
     int lock = -1;
     storage_error = true;
     if (state_directory(directory, sizeof(directory)) &&
         snprintf(path, sizeof(path), "%s/copilot-history", directory) < (int)sizeof(path) &&
         snprintf(lock_path, sizeof(lock_path), "%s/copilot-history.lock", directory) < (int)sizeof(lock_path)) {
          lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
          /* Never block the clock behind another instance's state write. */
          if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0) {
               load_history(path, &history);
               storage_error = false;
          } else if (lock >= 0) {
               close(lock);
               lock = -1;
          }
     }

     time_t month = month_start(now, 0), day = now - now % DAY;
     daily_sample *last = history.count ? &history.days[history.count - 1] : NULL;
     /* Resets, plan changes, corrections and clock rollback invalidate deltas. */
     if (history.month != month || history.total != total ||
         history.credits != credits ||
         (last && (now < last->last_at || used < last->last_used))) {
          history = (usage_history){.month = month, .total = total, .credits = credits};
          last = NULL;
     }
     if (!last || last->day != day) {
          double baseline = used;
          int partial = now != day;
          /* A reading spanning midnight by <=5 minutes approximates a full day. */
          if (last && last->day == day - DAY && now - last->last_at <= 300) {
               baseline = last->last_used;
               partial = 0;
          } else if (day == month && now - month <= 300) {
               baseline = 0;
               partial = 0;
          }
          if (history.count == HISTORY_DAYS) {
               memmove(history.days, history.days + 1,
                       sizeof(history.days[0]) * (HISTORY_DAYS - 1));
               --history.count;
          }
          last = &history.days[history.count++];
          *last = (daily_sample){day, now, now, baseline, used, used, partial};
     } else {
          last->last_at = now;
          last->last_used = used;
     }
     if (lock >= 0) {
          storage_error = !save_history(path);
          flock(lock, LOCK_UN);
          close(lock);
     }
}

void
copilot_budget_get(time_t now, copilot_forecast *forecast)
{
     *forecast = (copilot_forecast){0};
     if (!history.count || now < history.days[history.count - 1].last_at ||
         history.month != month_start(now, 0)) return;
     daily_sample *last = &history.days[history.count - 1];
     forecast->valid = true;
     forecast->storage_error = storage_error;
     forecast->reset_at = month_start(now, 1);
     forecast->remaining = fmax(0, history.total - last->last_used);
     forecast->safe_rate = forecast->remaining * DAY /
                           difftime(forecast->reset_at, last->last_at);
     time_t day = now - now % DAY;
     forecast->partial = last->partial || last->day != day;
     forecast->observed_since = last->day == day ? last->first_at : 0;
     double baseline = last->day == day ? last->baseline : last->last_used;
     forecast->today_used = fmax(0, last->last_used - baseline);
     forecast->daily_budget = (history.total - baseline) /
                             ((forecast->reset_at - day) / DAY);

     /* Use up to seven days of observed deltas, including time spent offline. */
     for (int i = 0; i < history.count; ++i) {
          daily_sample *first = &history.days[i];
          time_t elapsed = last->last_at - first->first_at;
          if (elapsed >= DAY && elapsed <= 7 * DAY) {
               forecast->daily_rate = (last->last_used - first->first_used) * DAY / elapsed;
               forecast->has_rate = true;
               break;
          }
     }
     if (!forecast->has_rate && last->last_at - history.month >= 3600) {
          forecast->daily_rate = last->last_used * DAY / (last->last_at - history.month);
          forecast->has_rate = true;
          forecast->monthly_rate = true;
     }
     if (forecast->has_rate)
          forecast->needed_until_reset = forecast->daily_rate *
                                        difftime(forecast->reset_at, last->last_at) / DAY;
     if (forecast->remaining == 0) {
          forecast->empty_at = last->last_at;
     } else if (forecast->has_rate) {
          double until_reset = difftime(forecast->reset_at, last->last_at);
          /* Anchor the estimate to the measurement, including when it is stale. */
          double seconds_left = forecast->daily_rate > 0
                                ? forecast->remaining / forecast->daily_rate * DAY : INFINITY;
          forecast->holds = seconds_left >= until_reset;
          if (!forecast->holds)
               forecast->empty_at = last->last_at + (time_t)seconds_left;
     }
}
