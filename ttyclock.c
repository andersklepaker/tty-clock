/*
 *      TTY-CLOCK Main file.
 *      Copyright © 2009-2018 tty-clock contributors
 *      Copyright © 2008 Martin Duquesnoy <xorg62@gmail.com>
 *      All rights reserved.
 *
 *      Redistribution and use in source and binary forms, with or without
 *      modification, are permitted provided that the following conditions are
 *      met:
 *
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following disclaimer
 *        in the documentation and/or other materials provided with the
 *        distribution.
 *      * Neither the name of the  nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 *      THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *      "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *      LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *      A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *      OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *      SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *      LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *      DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *      THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *      (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *      OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ttyclock.h"

/* Background, text, muted detail, then the eight selectable Nord accents. */
static const unsigned int nord_palette[] = {
     0x2e3440, 0xd8dee9, 0x4c566a,
     0x4c566a, 0xbf616a, 0xa3be8c, 0xebcb8b,
     0x81a1c1, 0xb48ead, 0x88c0d0, 0xeceff4
};
static short nord_colors[11];

static short
theme_color(unsigned int rgb, int slot)
{
     /* Round up so terminfo's conversion back to 8-bit RGB is lossless. */
     short r = (((rgb >> 16) & 255) * 1000 + 254) / 255;
     short g = (((rgb >> 8) & 255) * 1000 + 254) / 255;
     short b = ((rgb & 255) * 1000 + 254) / 255;
     if (COLORS >= 27 && can_change_color() &&
         init_color(16 + slot, r, g, b) == OK)
          return 16 + slot;

     /* Fixed palettes get the nearest available color, including ANSI-only terminals. */
     long best_distance = LONG_MAX;
     short best = COLOR_WHITE;
     for (int i = 0; i < COLORS && i < 256; ++i) {
          short cr, cg, cb;
          if (color_content(i, &cr, &cg, &cb) == ERR) continue;
          long dr = r - cr, dg = g - cg, db = b - cb;
          long distance = dr * dr + dg * dg + db * db;
          if (distance < best_distance) {
               best_distance = distance;
               best = i;
          }
     }
     return best;
}

static void
set_clock_color(int color)
{
     ttyclock.option.color = color;
     short accent = ttyclock.option.nord ? nord_colors[color + 3] : color;
     init_pair(1, ttyclock.bg, accent);
     init_pair(2, ttyclock.option.nord ? nord_colors[1] : accent, ttyclock.bg);
}

static void
init_colors(void)
{
     ttyclock.bg = use_default_colors() == OK ? -1 : COLOR_BLACK;
     if (ttyclock.option.nord) {
          for (int i = 0; i < 11; ++i)
               nord_colors[i] = theme_color(nord_palette[i], i);
          ttyclock.bg = nord_colors[0];
     }
     set_clock_color(ttyclock.option.color);
     init_pair(3, ttyclock.option.nord ? nord_colors[2] :
                  (ttyclock.bg == -1 ? -1 : COLOR_WHITE), ttyclock.bg);
     bkgd(COLOR_PAIR(3));
}

int
clock_height(void)
{
     return ttyclock.geo.h + (ttyclock.option.date ? DATEWINH - 1 : 0) +
            (ttyclock.option.copilot ? COPILOT_ROWS : 0);
}

/* Pads let the clock be clipped safely when the terminal is very small. */
static void
refresh_window(WINDOW *win)
{
     int y = ttyclock.geo.x, x = ttyclock.geo.y;
     int h = getmaxy(win), w = getmaxx(win);
     if (win == ttyclock.datewin) {
          y += ttyclock.geo.h - 1;
          x += ttyclock.geo.w / 2 - w / 2;
     } else if (win == ttyclock.usagewin) {
          y += ttyclock.geo.h + (ttyclock.option.date ? DATEWINH - 1 : 0);
          w = COLS < COPILOT_WIDTH ? COLS : COPILOT_WIDTH;
          x += ttyclock.geo.w / 2 - w / 2;
     }
     if (x < 0) x = 0;
     if (x + w > COLS && w <= COLS) x = COLS - w;
     if (y < 0 || y >= LINES || x >= COLS) return;
     if (y + h > LINES) h = LINES - y;
     if (x + w > COLS) w = COLS - x;
     if (h > 0 && w > 0)
          prefresh(win, 0, 0, y, x, y + h - 1, x + w - 1);
}

static void
draw_usage(void)
{
     if (!ttyclock.option.copilot) return;
     char lines[COPILOT_ROWS][256];
     int width = COLS < COPILOT_WIDTH ? COLS : COPILOT_WIDTH;
     copilot_display_lines(time(NULL), lines, width);
     werase(ttyclock.usagewin);
     wattrset(ttyclock.usagewin, COLOR_PAIR(2) |
              (ttyclock.option.bold ? A_BOLD : 0));
     for (int row = 0; row < COPILOT_ROWS; ++row) {
          const char *text = lines[row];
          int len = strlen(text);
          for (int i = 0; i < len && i < width; ++i) {
               chtype ch = (unsigned char)text[i];
               if ((row == 0 || row == COPILOT_TODAY_GRAPH_ROW) &&
                   copilot.valid && !copilot.unlimited) {
                    if (ch == '#') ch = ' ' | COLOR_PAIR(1);
                    if (ch == '-') ch = ACS_CKBOARD | A_DIM |
                                        (ttyclock.option.nord ? COLOR_PAIR(3) : 0);
               }
               mvwaddch(ttyclock.usagewin, row, i, ch);
          }
     }
     refresh_window(ttyclock.usagewin);
}

void
init(void)
{
     struct sigaction sig;
     setlocale(LC_TIME,"");

     ttyclock.bg = COLOR_BLACK;

     /* Init ncurses */
     if (ttyclock.tty) {
          FILE *ftty = fopen(ttyclock.tty, "r+");
          if (!ftty) {
               fprintf(stderr, "tty-clock: error: '%s' couldn't be opened: %s.\n",
                       ttyclock.tty, strerror(errno));
               exit(EXIT_FAILURE);
          }
          ttyclock.ttyscr = newterm(NULL, ftty, ftty);
          assert(ttyclock.ttyscr != NULL);
          set_term(ttyclock.ttyscr);
     } else
          initscr();

     cbreak();
     noecho();
     keypad(stdscr, true);
     start_color();
     curs_set(false);
     clear();

     init_colors();
     refresh();

     /* Init signal handler */
     sig.sa_handler = signal_handler;
     sig.sa_flags   = 0;
     sigemptyset(&sig.sa_mask);
     sigaction(SIGTERM,  &sig, NULL);
     sigaction(SIGINT,   &sig, NULL);
     sigaction(SIGSEGV,  &sig, NULL);

     /* Init global struct */
     ttyclock.running = true;
     if(!ttyclock.geo.x)
          ttyclock.geo.x = 0;
     if(!ttyclock.geo.y)
          ttyclock.geo.y = 0;
     if(!ttyclock.geo.a)
          ttyclock.geo.a = 1;
     if(!ttyclock.geo.b)
          ttyclock.geo.b = 1;
     ttyclock.geo.w = (ttyclock.option.second) ? SECFRAMEW : NORMFRAMEW;
     ttyclock.geo.h = 7;
     ttyclock.tm = localtime(&(ttyclock.lt));
     if(ttyclock.option.utc) {
          ttyclock.tm = gmtime(&(ttyclock.lt));
     }
     ttyclock.lt = time(NULL);
     update_hour();

     /* Create clock win */
     if (ttyclock.framewin) delwin(ttyclock.framewin);
     if (ttyclock.datewin) delwin(ttyclock.datewin);
     if (ttyclock.usagewin) delwin(ttyclock.usagewin);
     ttyclock.framewin = newpad(ttyclock.geo.h, ttyclock.geo.w);
     if (!ttyclock.framewin) {
          endwin();
          fprintf(stderr, "tty-clock: cannot allocate clock window\n");
          exit(EXIT_FAILURE);
     }
     wbkgd(ttyclock.framewin, COLOR_PAIR(3));
     if(ttyclock.option.box) {
          box(ttyclock.framewin, 0, 0);
     }

     if (ttyclock.option.bold)
     {
          wattron(ttyclock.framewin, A_BLINK);
     }

     /* Create the date win */
     ttyclock.datewin = newpad(DATEWINH, strlen(ttyclock.date.datestr) + 2);
     ttyclock.usagewin = newpad(COPILOT_ROWS, 256);
     if (!ttyclock.framewin || !ttyclock.datewin || !ttyclock.usagewin) {
          endwin();
          fprintf(stderr, "tty-clock: cannot allocate clock windows\n");
          exit(EXIT_FAILURE);
     }
     wbkgd(ttyclock.datewin, COLOR_PAIR(3));
     wbkgd(ttyclock.usagewin, COLOR_PAIR(3));
     if(ttyclock.option.box && ttyclock.option.date) {
          box(ttyclock.datewin, 0, 0);
     }
     clearok(ttyclock.datewin, true);

     set_center(ttyclock.option.center);
     clock_move(ttyclock.geo.x, ttyclock.geo.y, ttyclock.geo.w, ttyclock.geo.h);

     nodelay(stdscr, true);

     if (ttyclock.option.date)
     {
          refresh_window(ttyclock.datewin);
     }

     refresh_window(ttyclock.framewin);

     return;
}

void
signal_handler(int signal)
{
     switch(signal)
     {
     case SIGINT:
     case SIGTERM:
          ttyclock.running = false;
          break;
          /* Segmentation fault signal */
     case SIGSEGV:
          endwin();
          fprintf(stderr, "Segmentation fault.\n");
          exit(EXIT_FAILURE);
          break;
     }

     return;
}

void
cleanup(void)
{
     copilot_cleanup();
     if (ttyclock.ttyscr)
          delscreen(ttyclock.ttyscr);

     free(ttyclock.tty);
}

void
update_hour(void)
{
     int ihour;
     char tmpstr[128];

     ttyclock.lt = time(NULL);
     ttyclock.tm = localtime(&(ttyclock.lt));
     if(ttyclock.option.utc) {
          ttyclock.tm = gmtime(&(ttyclock.lt));
     }

     ihour = ttyclock.tm->tm_hour;

     if(ttyclock.option.twelve)
          ttyclock.meridiem = ((ihour >= 12) ? PMSIGN : AMSIGN);
     else
          ttyclock.meridiem = "\0";

     /* Manage hour for twelve mode */
     ihour = ((ttyclock.option.twelve && ihour > 12)  ? (ihour - 12) : ihour);
     ihour = ((ttyclock.option.twelve && !ihour) ? 12 : ihour);

     /* Set hour */
     ttyclock.date.hour[0] = ihour / 10;
     ttyclock.date.hour[1] = ihour % 10;

     /* Set minutes */
     ttyclock.date.minute[0] = ttyclock.tm->tm_min / 10;
     ttyclock.date.minute[1] = ttyclock.tm->tm_min % 10;

     /* Set date string */
     strcpy(ttyclock.date.old_datestr, ttyclock.date.datestr);
     strftime(tmpstr,
              sizeof(tmpstr),
              ttyclock.option.format,
              ttyclock.tm);
     sprintf(ttyclock.date.datestr, "%s%s", tmpstr, ttyclock.meridiem);

     /* Set seconds */
     ttyclock.date.second[0] = ttyclock.tm->tm_sec / 10;
     ttyclock.date.second[1] = ttyclock.tm->tm_sec % 10;

     return;
}

void
draw_number(int n, int x, int y)
{
     int i, sy = y;

     for(i = 0; i < 30; ++i, ++sy)
     {
          if(sy == y + 6)
          {
               sy = y;
               ++x;
          }

          if (ttyclock.option.bold)
               wattron(ttyclock.framewin, A_BLINK);
          else
               wattroff(ttyclock.framewin, A_BLINK);

          wbkgdset(ttyclock.framewin, COLOR_PAIR(number[n][i/2] ? 1 : 3));
          mvwaddch(ttyclock.framewin, x, sy, ' ');
     }
     refresh_window(ttyclock.framewin);

     return;
}

void
draw_clock(void)
{
     if (ttyclock.option.date && !ttyclock.option.rebound &&
               strcmp(ttyclock.date.datestr, ttyclock.date.old_datestr) != 0)
     {
          clock_move(ttyclock.geo.x,
                     ttyclock.geo.y,
                     ttyclock.geo.w,
                     ttyclock.geo.h);
     }

     /* Draw hour numbers */
     draw_number(ttyclock.date.hour[0], 1, 1);
     draw_number(ttyclock.date.hour[1], 1, 8);
     chtype dotcolor = COLOR_PAIR(1);
     if (ttyclock.option.blink && time(NULL) % 2 == 0)
          dotcolor = COLOR_PAIR(2);

     /* 2 dot for number separation */
     wbkgdset(ttyclock.framewin, dotcolor);
     mvwaddstr(ttyclock.framewin, 2, 16, "  ");
     mvwaddstr(ttyclock.framewin, 4, 16, "  ");

     /* Draw minute numbers */
     draw_number(ttyclock.date.minute[0], 1, 20);
     draw_number(ttyclock.date.minute[1], 1, 27);

     /* Draw the date */
     if (ttyclock.option.bold)
          wattron(ttyclock.datewin, A_BOLD);
     else
          wattroff(ttyclock.datewin, A_BOLD);

     if (ttyclock.option.date)
     {
          wbkgdset(ttyclock.datewin, (COLOR_PAIR(2)));
          mvwprintw(ttyclock.datewin, (DATEWINH / 2), 1, "%s", ttyclock.date.datestr);
          refresh_window(ttyclock.datewin);
     }

     /* Draw second if the option is enabled */
     if(ttyclock.option.second)
     {
          /* Again 2 dot for number separation */
          wbkgdset(ttyclock.framewin, dotcolor);
          mvwaddstr(ttyclock.framewin, 2, NORMFRAMEW, "  ");
          mvwaddstr(ttyclock.framewin, 4, NORMFRAMEW, "  ");

          /* Draw second numbers */
          draw_number(ttyclock.date.second[0], 1, 39);
          draw_number(ttyclock.date.second[1], 1, 46);
     }

     draw_usage();
     return;
}

void
clock_move(int x, int y, int w, int h)
{
     int max_x = LINES - clock_height();
     int max_y = COLS - w;
     if (ttyclock.option.copilot && COLS >= COPILOT_WIDTH && w < COPILOT_WIDTH) {
          int margin = (COPILOT_WIDTH - w + 1) / 2;
          if (y < margin) y = margin;
          max_y -= margin;
     }
     if (x > max_x) x = max_x;
     if (y > max_y) y = max_y;
     if (x < 0) x = 0;
     if (y < 0) y = 0;
     erase();
     refresh();
     wbkgdset(ttyclock.framewin, COLOR_PAIR(3));
     werase(ttyclock.framewin);

     if (ttyclock.option.date)
     {
          wbkgdset(ttyclock.datewin, COLOR_PAIR(3));
          wborder(ttyclock.datewin, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ');
          werase(ttyclock.datewin);
     }

     /* Frame win move */
     ttyclock.geo.x = x;
     ttyclock.geo.y = y;
     wresize(ttyclock.framewin, (ttyclock.geo.h = h), (ttyclock.geo.w = w));

     /* Date win move */
     if (ttyclock.option.date)
     {
          wresize(ttyclock.datewin, DATEWINH, strlen(ttyclock.date.datestr) + 2);

          if (ttyclock.option.box) {
               box(ttyclock.datewin,  0, 0);
          }
     }

     if (ttyclock.option.box)
     {
          box(ttyclock.framewin, 0, 0);
     }

     refresh_window(ttyclock.framewin);
     if (ttyclock.option.date) refresh_window(ttyclock.datewin);
     return;
}

/* Useless but fun :) */
void
clock_rebound(void)
{
     if(!ttyclock.option.rebound)
          return;

     if(ttyclock.geo.x < 1)
          ttyclock.geo.a = 1;
     if(ttyclock.geo.x >= (LINES - clock_height()))
          ttyclock.geo.a = -1;
     if(ttyclock.geo.y <= (ttyclock.option.copilot && ttyclock.geo.w < COPILOT_WIDTH ?
                            (COPILOT_WIDTH - ttyclock.geo.w + 1) / 2 : 0))
          ttyclock.geo.b = 1;
     if(ttyclock.geo.y >= (COLS - ttyclock.geo.w - 1 -
                             (ttyclock.option.copilot && ttyclock.geo.w < COPILOT_WIDTH ?
                              (COPILOT_WIDTH - ttyclock.geo.w + 1) / 2 : 0)))
          ttyclock.geo.b = -1;

     clock_move(ttyclock.geo.x + ttyclock.geo.a,
                ttyclock.geo.y + ttyclock.geo.b,
                ttyclock.geo.w,
                ttyclock.geo.h);

     return;
}

void
set_second(void)
{
     int new_w = (((ttyclock.option.second = !ttyclock.option.second)) ? SECFRAMEW : NORMFRAMEW);
     int y_adj;

     for(y_adj = 0; (ttyclock.geo.y - y_adj) > (COLS - new_w - 1); ++y_adj);

     clock_move(ttyclock.geo.x, (ttyclock.geo.y - y_adj), new_w, ttyclock.geo.h);

     set_center(ttyclock.option.center);

     return;
}

void
set_center(bool b)
{
     if((ttyclock.option.center = b))
     {
          ttyclock.option.rebound = false;

          clock_move((LINES / 2 - ((ttyclock.option.copilot ? clock_height() : ttyclock.geo.h) / 2)),
                     (COLS  / 2 - (ttyclock.geo.w / 2)),
                     ttyclock.geo.w,
                     ttyclock.geo.h);
     }

     return;
}

void
set_box(bool b)
{
     ttyclock.option.box = b;

     wbkgdset(ttyclock.framewin, COLOR_PAIR(3));
     wbkgdset(ttyclock.datewin, COLOR_PAIR(3));

     if(ttyclock.option.box) {
          wbkgdset(ttyclock.framewin, COLOR_PAIR(3));
          wbkgdset(ttyclock.datewin, COLOR_PAIR(3));
          box(ttyclock.framewin, 0, 0);
          box(ttyclock.datewin,  0, 0);
     }
     else {
          wborder(ttyclock.framewin, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ');
          wborder(ttyclock.datewin, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ');
     }

     if (ttyclock.option.date) refresh_window(ttyclock.datewin);
     refresh_window(ttyclock.framewin);
}

void
key_event(void)
{
     int i, c;

     struct timespec length = { ttyclock.option.delay, ttyclock.option.nsdelay };
     /* Poll fetch completion and enforce its timeout even with a long -d. */
     if (ttyclock.option.copilot && length.tv_sec > 1) {
          length.tv_sec = 1;
          length.tv_nsec = 0;
     }
     
     fd_set rfds;
     FD_ZERO(&rfds);
     FD_SET(STDIN_FILENO, &rfds);

     if (ttyclock.option.screensaver)
     {
          c = wgetch(stdscr);
          if(c != ERR && ttyclock.option.noquit == false)
          {
               ttyclock.running = false;
          }
          else
          {
               nanosleep(&length, NULL);
               for(i = 0; i < 8; ++i)
                    if(c == (i + '0'))
                    {
                         set_clock_color(i);
                    }
          }
          return;
     }


     switch(c = wgetch(stdscr))
     {
     case KEY_RESIZE:
          endwin();
          init();
          break;

     case KEY_UP:
     case 'k':
     case 'K':
          if(ttyclock.geo.x >= 1
             && !ttyclock.option.center)
               clock_move(ttyclock.geo.x - 1, ttyclock.geo.y, ttyclock.geo.w, ttyclock.geo.h);
          break;

     case KEY_DOWN:
     case 'j':
     case 'J':
          if(ttyclock.geo.x < (LINES - clock_height())
             && !ttyclock.option.center)
               clock_move(ttyclock.geo.x + 1, ttyclock.geo.y, ttyclock.geo.w, ttyclock.geo.h);
          break;

     case KEY_LEFT:
     case 'h':
     case 'H':
          if(ttyclock.geo.y >= 1
             && !ttyclock.option.center)
               clock_move(ttyclock.geo.x, ttyclock.geo.y - 1, ttyclock.geo.w, ttyclock.geo.h);
          break;

     case KEY_RIGHT:
     case 'l':
     case 'L':
          if(ttyclock.geo.y <= (COLS - ttyclock.geo.w - 1)
             && !ttyclock.option.center)
               clock_move(ttyclock.geo.x, ttyclock.geo.y + 1, ttyclock.geo.w, ttyclock.geo.h);
          break;

     case 'q':
     case 'Q':
          if (ttyclock.option.noquit == false)
               ttyclock.running = false;
          break;

     case 's':
     case 'S':
          set_second();
          break;

     case 'p':
     case 'P':
          ttyclock.option.copilot = !ttyclock.option.copilot;
          set_center(ttyclock.option.center);
          clock_move(ttyclock.geo.x, ttyclock.geo.y, ttyclock.geo.w, ttyclock.geo.h);
          break;

     case 't':
     case 'T':
          ttyclock.option.twelve = !ttyclock.option.twelve;
          /* Set the new ttyclock.date.datestr to resize date window */
          update_hour();
          clock_move(ttyclock.geo.x, ttyclock.geo.y, ttyclock.geo.w, ttyclock.geo.h);
          break;

     case 'c':
     case 'C':
          set_center(!ttyclock.option.center);
          break;

     case 'b':
     case 'B':
          ttyclock.option.bold = !ttyclock.option.bold;
          break;

     case 'r':
     case 'R':
          ttyclock.option.rebound = !ttyclock.option.rebound;
          if(ttyclock.option.rebound && ttyclock.option.center)
               ttyclock.option.center = false;
          break;

     case 'x':
     case 'X':
          set_box(!ttyclock.option.box);
          break;

     case '0': case '1': case '2': case '3':
     case '4': case '5': case '6': case '7':
          i = c - '0';
          set_clock_color(i);
          break;

     default:
          pselect(1, &rfds, NULL, NULL, &length, NULL);
     }

     return;
}

int
main(int argc, char **argv)
{
     int c;
     bool color_selected = false;
     static const struct option long_options[] = {
          {"copilot", no_argument, NULL, 256},
          {"copilot-refresh", required_argument, NULL, 257},
          {"theme", required_argument, NULL, 258},
          {NULL, 0, NULL, 0}
     };

     /* Alloc ttyclock */
     memset(&ttyclock, 0, sizeof(ttyclock_t));

     ttyclock.option.date = true;
     ttyclock.option.copilot_refresh = 60;

     /* Default date format */
     strncpy(ttyclock.option.format, "%F", sizeof (ttyclock.option.format));
     /* Default color */
     ttyclock.option.color = COLOR_GREEN; /* COLOR_GREEN = 2 */
     /* Default delay */
     ttyclock.option.delay = 1; /* 1FPS */
     ttyclock.option.nsdelay = 0; /* -0FPS */
     ttyclock.option.blink = false;

     atexit(cleanup);

     while ((c = getopt_long(argc, argv, "iuvsScbtrhBxnDC:f:d:T:a:", long_options, NULL)) != -1)
     {
          switch(c)
          {
          case 256:
               ttyclock.option.copilot = true;
               break;
          case 257: {
               char *end;
               errno = 0;
               unsigned long interval = strtoul(optarg, &end, 10);
               if (errno || end == optarg || *end || interval < 1 || interval > 86400) {
                    fprintf(stderr, "tty-clock: --copilot-refresh must be 1..86400 seconds\n");
                    return EXIT_FAILURE;
               }
               ttyclock.option.copilot_refresh = interval;
               break;
          }
          case 258:
               if (!strcmp(optarg, "nord"))
                    ttyclock.option.nord = true;
               else if (!strcmp(optarg, "default"))
                    ttyclock.option.nord = false;
               else {
                    fprintf(stderr, "tty-clock: unknown theme '%s' (use default or nord)\n", optarg);
                    return EXIT_FAILURE;
               }
               break;
          case 'h':
          default:
               printf("usage : tty-clock [-iuvsScbtrahDBxn] [-C [0-7]] [-f format] [-d delay] [-a nsdelay] [-T tty] [--theme default|nord] [--copilot] [--copilot-refresh seconds]\n"
                      "    --theme name  Set the color theme (default or nord)\n"
                      "    --copilot     Show Copilot usage below the date (P toggles)   \n"
                      "    --copilot-refresh seconds  Refresh interval, default 60 (1..86400)\n"
                      "    -s            Show seconds                                   \n"
                      "    -S            Screensaver mode                               \n"
                      "    -x            Show box                                       \n"
                      "    -c            Set the clock at the center of the terminal    \n"
                      "    -C [0-7]      Set the clock color                            \n"
                      "    -b            Use bold colors                                \n"
                      "    -t            Set the hour in 12h format                     \n"
                      "    -u            Use UTC time                                   \n"
                      "    -T tty        Display the clock on the specified terminal    \n"
                      "    -r            Do rebound the clock                           \n"
                      "    -f format     Set the date format                            \n"
                      "    -n            Don't quit on keypress                         \n"
                      "    -v            Show tty-clock version                         \n"
                      "    -i            Show some info about tty-clock                 \n"
                      "    -h            Show this page                                 \n"
                      "    -D            Hide date                                      \n"
                      "    -B            Enable blinking colon                          \n"
                      "    -d delay      Set the delay between two redraws of the clock. Default 1s. \n"
                      "    -a nsdelay    Additional delay between two redraws in nanoseconds. Default 0ns.\n");
               exit(EXIT_SUCCESS);
               break;
          case 'i':
               puts("TTY-Clock 2 © by Martin Duquesnoy (xorg62@gmail.com), Grey (grey@greytheory.net)");
               exit(EXIT_SUCCESS);
               break;
          case 'u':
               ttyclock.option.utc = true;
               break;
          case 'v':
               puts("TTY-Clock 2 © devel version");
               exit(EXIT_SUCCESS);
               break;
          case 's':
               ttyclock.option.second = true;
               break;
          case 'S':
               ttyclock.option.screensaver = true;
               break;
          case 'c':
               ttyclock.option.center = true;
               break;
          case 'b':
               ttyclock.option.bold = true;
               break;
          case 'C':
               if(atoi(optarg) >= 0 && atoi(optarg) < 8) {
                    ttyclock.option.color = atoi(optarg);
                    color_selected = true;
               }
               break;
          case 't':
               ttyclock.option.twelve = true;
               break;
          case 'r':
               ttyclock.option.rebound = true;
               break;
          case 'f':
               strncpy(ttyclock.option.format, optarg, 100);
               break;
          case 'd':
               if(atol(optarg) >= 0 && atol(optarg) < 100)
                    ttyclock.option.delay = atol(optarg);
               break;
          case 'D':
               ttyclock.option.date = false;
               break;
          case 'B':
               ttyclock.option.blink = true;
               break;
          case 'a':
               if(atol(optarg) >= 0 && atol(optarg) < 1000000000)
                    ttyclock.option.nsdelay = atol(optarg);
               break;
          case 'x':
               ttyclock.option.box = true;
               break;
          case 'T': {
               struct stat sbuf;
               if (stat(optarg, &sbuf) == -1) {
                    fprintf(stderr, "tty-clock: error: couldn't stat '%s': %s.\n",
                              optarg, strerror(errno));
                    exit(EXIT_FAILURE);
               } else if (!S_ISCHR(sbuf.st_mode)) {
                    fprintf(stderr, "tty-clock: error: '%s' doesn't appear to be a character device.\n",
                              optarg);
                    exit(EXIT_FAILURE);
               } else {
                    free(ttyclock.tty);
                    ttyclock.tty = strdup(optarg);
               }}
               break;
          case 'n':
               ttyclock.option.noquit = true;
               break;
          }
     }

     if (ttyclock.option.nord && !color_selected)
          ttyclock.option.color = 6; /* Nord Frost */
     init();
     attron(A_BLINK);
     while(ttyclock.running)
     {
          copilot_tick(ttyclock.option.copilot, ttyclock.option.copilot_refresh);
          clock_rebound();
          update_hour();
          draw_clock();
          key_event();
     }

     endwin();

     return 0;
}

// vim: expandtab tabstop=5 softtabstop=5 shiftwidth=5
