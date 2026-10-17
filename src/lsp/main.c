/*
 * t-code-lsp: what micro's lsp plugin starts instead of a language server inside t-code.
 *
 *   t-code-lsp <server> [args...]
 *
 * Connects to t-code's language server socket ($TCODE_LSP), says which server it wants and from
 * which folder (one line: "<cwd>\t<server>\t<arg>...\n") and then copies bytes both ways:
 * stdin to the socket, the socket to stdout. t-code runs one server per folder and command for
 * all its editors (src/ipc/lsp.c). Without t-code, or when it cannot be reached, it runs the server
 * itself, like the plugin would have.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static int connect_to(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd;

    if (strlen(path) >= sizeof address.sun_path)
        return -1;
    strcpy(address.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* "<cwd>\t<server>\t<arg>...\n"; tabs and newlines cannot be told apart from the separators. */
static int send_header(int fd, char **argv)
{
    char cwd[PATH_MAX];

    if (!getcwd(cwd, sizeof cwd) || strpbrk(cwd, "\t\n"))
        return -1;
    if (write_all(fd, cwd, strlen(cwd)) < 0)
        return -1;
    for (int i = 0; argv[i]; i++) {
        if (strpbrk(argv[i], "\t\n") || write_all(fd, "\t", 1) < 0 ||
            write_all(fd, argv[i], strlen(argv[i])) < 0)
            return -1;
    }
    return write_all(fd, "\n", 1);
}

/* Copies until either side closes. */
static void relay(int sock)
{
    struct pollfd fds[2] = { { .fd = STDIN_FILENO, .events = POLLIN }, { .fd = sock, .events = POLLIN } };
    char buf[65536];

    for (;;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        for (int i = 0; i < 2; i++) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0 || write_all(i == 0 ? sock : STDOUT_FILENO, buf, (size_t)n) < 0)
                return;
        }
    }
}

int main(int argc, char **argv)
{
    const char *path = getenv("TCODE_LSP");
    int sock;

    if (argc < 2) {
        fprintf(stderr, "usage: t-code-lsp <server> [args...]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    sock = path && *path ? connect_to(path) : -1;
    if (sock < 0 || send_header(sock, argv + 1) < 0) {
        if (sock >= 0)
            close(sock);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[1], argv + 1);
        fprintf(stderr, "t-code-lsp: cannot run %s: %s\n", argv[1], strerror(errno));
        return 127;
    }
    relay(sock);
    close(sock);
    return 0;
}
