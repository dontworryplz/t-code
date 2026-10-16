/* t-code-fm's main loop: draws, then waits for keys, mouse, resizes and file changes. */
#include "fm.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define REFRESH_DELAY_MS 150 /* changes come in bursts (a save, a git commit): refresh once */

Fm fm = { .show_hidden = true, .focused = true, .follow = true, .press_row = -1, .drop_row = -1 };
volatile int resized;

static void restore_terminal(void)
{
    static const char leave[] = "\x1b[?1004l\x1b[?1006l\x1b[?1002l\x1b[?25h\x1b[?1049l";

    if (write(STDOUT_FILENO, leave, sizeof leave - 1) < 0) {
        /* Ignore: the terminal is gone. */
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &fm.saved);
}

/* Raw keys, the alternate screen, mouse buttons with dragging (1002, SGR 1006) and focus events. */
static void setup_terminal(void)
{
    static const char enter[] = "\x1b[?1049h\x1b[?1002h\x1b[?1006h\x1b[?1004h\x1b[?25l";
    struct termios raw;

    tcgetattr(STDIN_FILENO, &fm.saved);
    atexit(restore_terminal);
    raw = fm.saved;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    if (write(STDOUT_FILENO, enter, sizeof enter - 1) < 0)
        exit(1);
}

static void on_winch(int sig)
{
    (void)sig;
    resized = 1;
}

int main(void)
{
    struct sigaction sa = { .sa_handler = on_winch }; /* no SA_RESTART: poll() returns on resize */
    unsigned char buf[512];
    bool refresh_pending = false;

    if (!getcwd(fm.root, sizeof fm.root)) {
        perror("t-code-fm");
        return 1;
    }
    sigaction(SIGWINCH, &sa, NULL);
    setup_terminal();
    update_size();
    rebuild(NULL);

    for (;;) {
        struct pollfd fds[2] = { { .fd = STDIN_FILENO, .events = POLLIN }, { .fd = watch_fd(), .events = POLLIN } };

        draw(NULL, NULL);
        int ready = poll(fds, fds[1].fd >= 0 ? 2 : 1, refresh_pending ? REFRESH_DELAY_MS : -1);
        if (ready < 0) {
            if (errno != EINTR)
                break;
            if (resized) {
                resized = 0;
                update_size();
            }
            continue;
        }
        if (ready == 0) { /* the burst of changes is over */
            refresh_pending = false;
            rebuild(selected_path());
            continue;
        }
        if (fds[1].fd >= 0 && (fds[1].revents & POLLIN) && watch_read())
            refresh_pending = true;
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            int n = read_input(buf, sizeof buf);
            if (n < 0)
                break;
            handle_input(buf, n);
        }
    }
    return 0;
}
