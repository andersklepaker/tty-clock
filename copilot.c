#include "copilot.h"
#include "copilot_budget.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

copilot_usage copilot;
static pid_t child;
static int output = -1;
static char response[512];
static size_t received;
static double started, next_fetch;

static double
monotonic_seconds(void)
{
     struct timespec now;
     clock_gettime(CLOCK_MONOTONIC, &now);
     return now.tv_sec + now.tv_nsec / 1000000000.0;
}

void
copilot_cleanup(void)
{
     if (child > 0) {
          kill(-child, SIGKILL);
          kill(child, SIGKILL);
          while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
          child = 0;
     }
     if (output >= 0) {
          close(output);
          output = -1;
     }
}

static bool
parse_response(void)
{
     copilot_usage parsed = {0};
     double remaining, credits_used;
     int unlimited, credits, has_quota, consumed = 0;
     if (sscanf(response, "quota %lf %lf %lf %lf %d %d %d %n",
                &remaining, &parsed.total, &parsed.used, &credits_used,
                &unlimited, &credits, &has_quota, &consumed) != 7 ||
         response[consumed] != '\0' || !isfinite(remaining) ||
         !isfinite(parsed.total) || !isfinite(parsed.used) ||
         !isfinite(credits_used) || remaining < 0 || remaining > 100 ||
         (unlimited != 0 && unlimited != 1) ||
         (credits != 0 && credits != 1) ||
         (has_quota != 0 && has_quota != 1))
          return false;

     /* The API's quota_remaining shares the entitlement's units. */
     parsed.used = !unlimited && parsed.total > 0 && parsed.used >= 0
                   ? fmax(0, parsed.total - parsed.used) : credits_used;
     parsed.valid = true;
     parsed.unlimited = unlimited;
     parsed.credits = credits;
     parsed.has_quota = has_quota;
     parsed.percent_used = 100 - remaining;
     copilot = parsed;
     if (!copilot.unlimited && copilot.has_quota && copilot.total > 0)
          copilot_budget_sample(time(NULL), copilot.used, copilot.total, copilot.credits);
     return true;
}

void
copilot_tick(bool enabled, unsigned int interval)
{
     double now = monotonic_seconds();
     if (!enabled) {
          copilot_cleanup();
          return;
     }
     if (child > 0) {
          ssize_t n;
          while (received < sizeof(response) - 1 &&
                 (n = read(output, response + received,
                           sizeof(response) - 1 - received)) > 0)
               received += n;
          int status;
          pid_t result = waitpid(child, &status, WNOHANG);
          if (result == child) {
               /* Drain anything written between the first read and exit. */
               while (received < sizeof(response) - 1 &&
                      (n = read(output, response + received,
                                sizeof(response) - 1 - received)) > 0)
                    received += n;
               response[received] = '\0';
               child = 0;
               close(output);
               output = -1;
               if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
                   received == sizeof(response) - 1 || !parse_response())
                    copilot.stale = true;
               next_fetch = started + interval;
          } else if (now - started >= 15 || result < 0) {
               copilot_cleanup();
               copilot.stale = true;
               next_fetch = started + interval;
          }
          return;
     }
     if (now < next_fetch)
          return;

     int fds[2];
     if (pipe(fds) < 0) {
          copilot.stale = true;
          next_fetch = now + interval;
          return;
     }
     child = fork();
     if (child == 0) {
          setpgid(0, 0);
          dup2(fds[1], STDOUT_FILENO);
          close(fds[0]);
          close(fds[1]);
          int null = open("/dev/null", O_RDWR);
          if (null >= 0) {
               dup2(null, STDIN_FILENO);
               dup2(null, STDERR_FILENO);
               if (null > STDERR_FILENO) close(null);
          }
          setenv("GH_PROMPT_DISABLED", "1", 1);
          setenv("GH_PAGER", "cat", 1);
          execlp("gh", "gh", "api", "--hostname", "github.com",
                 "/copilot_internal/user",
                 "-H", "Cache-Control: no-cache", "-H", "Pragma: no-cache", "--jq",
                 ". as $u | .quota_snapshots.premium_interactions | "
                 "select(. != null) | "
                 "[\"quota\", (.percent_remaining // -1), (.entitlement // -1), "
                 "(.quota_remaining // -1), (.credits_used // -1), "
                 "(if .unlimited then 1 else 0 end), "
                 "(if (.token_based_billing // $u.token_based_billing // false) then 1 else 0 end), "
                 "(if .has_quota == false then 0 else 1 end)] | @tsv",
                 (char *)NULL);
          _exit(127);
     }
     close(fds[1]);
     if (child < 0) {
          child = 0;
          close(fds[0]);
          copilot.stale = true;
          next_fetch = now + interval;
          return;
     }
     setpgid(child, child);
     output = fds[0];
     fcntl(output, F_SETFL, O_NONBLOCK);
     fcntl(output, F_SETFD, FD_CLOEXEC);
     received = 0;
     started = now;
}

static void
format_amount(double value, char *out, size_t size)
{
     char raw[64];
     snprintf(raw, sizeof(raw), "%.1f", value);
     char *dot = strchr(raw, '.');
     size_t integer = dot ? (size_t)(dot - raw) : strlen(raw), pos = 0;
     for (size_t i = 0; i < integer && pos + 1 < size; ++i) {
          if (i && (integer - i) % 3 == 0 && pos + 1 < size)
               out[pos++] = ',';
          if (pos + 1 < size) out[pos++] = raw[i];
     }
     if (dot && dot[1] != '0' && pos + 2 < size) {
          out[pos++] = '.';
          out[pos++] = dot[1];
     }
     out[pos] = '\0';
}

void
copilot_lines(char *bar, size_t bar_size, char *detail, size_t detail_size,
              int width)
{
     const char *stale = copilot.stale ? " (stale)" : "";
     detail[0] = '\0';
     if (!copilot.valid) {
          snprintf(bar, bar_size, "Copilot: %s", copilot.stale ? "unavailable" : "loading...");
          if (copilot.stale)
               snprintf(detail, detail_size, "Check gh auth login (github.com)");
          return;
     }
     if (copilot.unlimited) {
          snprintf(bar, bar_size, "Copilot: %s%s",
                   copilot.has_quota ? "unlimited" : "quota unavailable", stale);
     } else {
          char suffix[64];
          snprintf(suffix, sizeof(suffix), " %.1f%% used%s", copilot.percent_used, stale);
          int cells = width - 10 - (int)strlen(suffix);
          if (cells > 20) cells = 20;
          if (cells < 1) {
               snprintf(bar, bar_size, "Copilot %.1f%% used%s", copilot.percent_used, stale);
          } else {
               char blocks[21];
               int filled = (int)(cells * copilot.percent_used / 100 + 0.5);
               for (int i = 0; i < cells; ++i) blocks[i] = i < filled ? '#' : '-';
               blocks[cells] = '\0';
               snprintf(bar, bar_size, "Copilot [%s]%s", blocks, suffix);
          }
     }
     char used[96], total[96];
     if (copilot.used >= 0) {
          format_amount(copilot.used, used, sizeof(used));
          if (!copilot.unlimited && copilot.total > 0) {
               format_amount(copilot.total, total, sizeof(total));
               snprintf(detail, detail_size, "%s / %s %s", used, total,
                        copilot.credits ? "credits" : "requests");
          } else {
               snprintf(detail, detail_size, "%s %s used", used,
                        copilot.credits ? "credits" : "requests");
          }
     }
}

static void
today_graph(const copilot_forecast *forecast, char *line, size_t size, int width)
{
     if (forecast->partial && !forecast->observed_since) {
          snprintf(line, size, "Today graph: waiting for a fresh reading");
          return;
     }
     if (!forecast->has_rate || forecast->daily_rate <= 0) {
          snprintf(line, size, "Today graph: daily average not available");
          return;
     }
     char label[32] = "Today", suffix[64], blocks[21];
     if (forecast->partial) {
          struct tm utc;
          gmtime_r(&forecast->observed_since, &utc);
          strftime(label, sizeof(label), "Since %H:%M", &utc);
     }
     double percent = forecast->today_used / forecast->daily_rate * 100;
     snprintf(suffix, sizeof(suffix), " %.1f%% of daily avg", percent);
     int cells = width - (int)strlen(label) - 3 - (int)strlen(suffix);
     if (cells > 20) cells = 20;
     if (cells < 1) {
          snprintf(line, size, "%s: %.1f%% of daily avg", label, percent);
          return;
     }
     int filled = (int)(cells * fmin(percent, 100) / 100 + 0.5);
     for (int i = 0; i < cells; ++i) blocks[i] = i < filled ? '#' : '-';
     blocks[cells] = '\0';
     snprintf(line, size, "%s [%s]%s", label, blocks, suffix);
}

void
copilot_forecast_lines(time_t now, char (*lines)[256], int width)
{
     memset(lines, 0, COPILOT_FORECAST_ROWS * 256);
     if (!copilot.valid || copilot.unlimited || !copilot.has_quota) return;
     if (copilot.total <= 0 || copilot.used < 0 || copilot.used > copilot.total) {
          snprintf(lines[0], 256, "Budget: quota amount unavailable");
          return;
     }
     copilot_forecast forecast;
     copilot_budget_get(now, &forecast);
     if (!forecast.valid) {
          snprintf(lines[0], 256, "Budget: waiting for current month's quota");
          return;
     }
     char used[64], budget[64], amount[64], date[32];
     struct tm utc;
     format_amount(forecast.today_used, used, sizeof(used));
     format_amount(forecast.daily_budget, budget, sizeof(budget));
     double left = forecast.daily_budget - forecast.today_used;
     if (forecast.partial && forecast.observed_since) {
          gmtime_r(&forecast.observed_since, &utc);
          strftime(date, sizeof(date), "%H:%M", &utc);
          snprintf(lines[0], 256, "Today: unknown; %s since %s UTC", used, date);
     } else if (forecast.partial)
          snprintf(lines[0], 256, "Today: unknown; waiting for a fresh reading");
     else if (left < 0 && forecast.daily_budget > 0)
          snprintf(lines[0], 256, "Today UTC: ~%s / %s (%.0f%% over)", used, budget,
                   -left / forecast.daily_budget * 100);
     else if (left < 0)
          snprintf(lines[0], 256, "Today UTC: ~%s (no daily budget left)", used);
     else {
          format_amount(left, amount, sizeof(amount));
          snprintf(lines[0], 256, "Today UTC: ~%s / %s (%s left)", used, budget, amount);
     }

     today_graph(&forecast, lines[1], 256, width);
     format_amount(forecast.safe_rate, amount, sizeof(amount));
     snprintf(lines[2], 256, "Safe avg: %s %s/day%s", amount,
              copilot.credits ? "credits" : "requests",
              forecast.storage_error ? " [no save]" : "");
     const char *rate = forecast.monthly_rate ? "Month avg" : "Recent avg";
     format_amount(forecast.daily_rate, amount, sizeof(amount));
     if (!forecast.has_rate)
          snprintf(lines[3], 256, "Average: collecting usage data");
     else if (forecast.remaining == 0)
          snprintf(lines[3], 256, "%s: %s/day (quota empty)", rate, amount);
     else if (forecast.daily_rate > forecast.safe_rate)
          snprintf(lines[3], 256, "%s: %s/day (%.1f%% over safe pace)", rate, amount,
                   (forecast.daily_rate / forecast.safe_rate - 1) * 100);
     else
          snprintf(lines[3], 256, "%s: %s/day (within budget)", rate, amount);

     gmtime_r(&forecast.reset_at, &utc);
     strftime(date, sizeof(date), "%d %b", &utc);
     if (forecast.has_rate) {
          double extra = fmax(0, forecast.needed_until_reset - forecast.remaining);
          format_amount(copilot.total + extra, amount, sizeof(amount));
          if (extra > 0)
               snprintf(lines[4], 256, "Est. limit by %s: %s %s (+%.1f%%)", date, amount,
                        copilot.credits ? "credits" : "requests", extra / copilot.total * 100);
          else
               snprintf(lines[4], 256, "Est. limit by %s: %s %s (no increase)", date, amount,
                        copilot.credits ? "credits" : "requests");
     } else
          snprintf(lines[4], 256, "Est. limit by %s: collecting usage data", date);
     if (forecast.remaining == 0)
          snprintf(lines[5], 256, "Quota empty; reset %s 00:00 UTC", date);
     else if (!forecast.has_rate)
          snprintf(lines[5], 256, "Reset: %s 00:00 UTC; forecast pending", date);
     else if (forecast.holds)
          snprintf(lines[5], 256, "At this pace: lasts until %s reset", date);
     else {
          gmtime_r(&forecast.empty_at, &utc);
          strftime(date, sizeof(date), "%d %b %H:%M", &utc);
          double gap = difftime(forecast.reset_at, forecast.empty_at);
          bool days = gap >= 86400;
          int early = (int)ceil(gap / (days ? 86400 : 3600));
          snprintf(lines[5], 256, "Empty: %s UTC; ~%d %s early", date, early,
                   days ? (early == 1 ? "day" : "days") : (early == 1 ? "hour" : "hours"));
     }
}

void
copilot_display_lines(time_t now, char (*lines)[256], int width)
{
     char forecast[COPILOT_FORECAST_ROWS][256];
     memset(lines, 0, COPILOT_ROWS * 256);
     copilot_lines(lines[0], 256, lines[1], 256, width);
     copilot_forecast_lines(now, forecast, width);
     /* Separate quota, today's usage and forecast with blank rows. */
     memcpy(&lines[3], &forecast[0], 2 * 256);
     memcpy(&lines[6], &forecast[2], 4 * 256);
}
