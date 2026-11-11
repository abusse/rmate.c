#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define HOST_ENV "RMATE_HOST"
#define PORT_ENV "RMATE_PORT"
#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT "52698"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#define PROTO_BUF_SIZE (PATH_MAX + 256)

int verbose = 0;

enum CMD_STATE
{
    CMD_HEADER,
    CMD_CMD,
    CMD_VAR
};

struct cmd
{
    enum CMD_STATE state;
    char* filename;
    /* The tokens of the files this session has sent to the editor. */
    char* const* files;
    int n_files;
};

enum log_type
{
    INFO,
    WARNING,
    ERROR
};

static const struct
{
    const char* label;
    const char* color;
} log_styles[] = {
    [INFO] = { "Info:", "\033[32m" },
    [WARNING] = { "Warning:", "\033[33m" },
    [ERROR] = { "Error:", "\033[31m" },
};

void
log_msg(enum log_type type, const char* fmt, ...)
{
    if (type == INFO && !verbose) {
        return;
    }

    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "%s%s\033[0m ", log_styles[type].color, log_styles[type].label);
    } else {
        fprintf(stderr, "%s ", log_styles[type].label);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

char*
read_stdin(size_t* out_len)
{
    size_t cap = 4096, len = 0;
    char* buf = malloc(cap);

    if (!buf) {
        return NULL;
    }

    while (1) {
        ssize_t n = read(STDIN_FILENO, buf + len, cap - len);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        len += n;
        if (len == cap) {
            char* tmp;
            size_t new_cap = cap * 2;

            /* Detect size_t overflow from doubling. */
            if (new_cap <= cap) {
                free(buf);
                errno = ENOMEM;
                return NULL;
            }
            cap = new_cap;
            tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }
    }

    *out_len = len;
    return buf;
}

int
write_all(int fd, const char* buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

/* Reads exactly len bytes. Fails if the other side closes the connection first. */
int
read_all(int fd, char* buf, size_t len)
{
    size_t missing = len;

    while (missing > 0) {
        ssize_t n = read(fd, buf, missing);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            log_msg(ERROR, "Failed to read data: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            log_msg(ERROR, "Connection closed with %zu of %zu bytes still missing", missing, len);
            return -1;
        }
        buf += n;
        missing -= n;
    }
    return 0;
}

/* Copies len bytes from from_fd to to_fd. */
int
stream_data(int from_fd, int to_fd, size_t len)
{
    char tmp[BUFSIZ];

    while (len > 0) {
        size_t to_read = len < sizeof(tmp) ? len : sizeof(tmp);
        ssize_t n = read(from_fd, tmp, to_read);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            log_msg(ERROR, "Failed to read data: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            log_msg(ERROR, "Unexpected EOF (%zu bytes remaining)", len);
            return -1;
        }
        if (write_all(to_fd, tmp, n) == -1) {
            log_msg(ERROR, "Failed to write data: %s", strerror(errno));
            return -1;
        }
        len -= n;
    }
    return 0;
}

int
send_open(int sockfd,
          const char* display_name,
          const char* real_path,
          const char* token,
          int data_fd,
          const char* data_buf,
          size_t data_len,
          int line)
{
    char hdr[HOST_NAME_MAX + PATH_MAX * 3 + 256];

    log_msg(INFO, "Opening %s (%zu bytes)", display_name, data_len);

    int off = snprintf(hdr,
                       sizeof(hdr),
                       "open\n"
                       "display-name: %s\n"
                       "real-path: %s\n"
                       "data-on-save: yes\n"
                       "re-activate: yes\n"
                       "token: %s\n",
                       display_name,
                       real_path,
                       token);

    if (line > 0 && off >= 0 && (size_t)off < sizeof(hdr)) {
        off += snprintf(hdr + off, sizeof(hdr) - off, "selection: %d\n", line);
    }

    if (off >= 0 && (size_t)off < sizeof(hdr)) {
        off += snprintf(hdr + off, sizeof(hdr) - off, "data: %zu\n", data_len);
    }

    if (off < 0 || (size_t)off >= sizeof(hdr)) {
        log_msg(ERROR, "Protocol header too long");
        return -1;
    }

    if (write_all(sockfd, hdr, off) == -1) {
        log_msg(ERROR, "Failed to send header to editor: %s", strerror(errno));
        return -1;
    }

    if (data_buf) {
        if (data_len > 0 && write_all(sockfd, data_buf, data_len) == -1) {
            log_msg(ERROR, "Failed to send file data to editor: %s", strerror(errno));
            return -1;
        }
    } else if (data_fd >= 0) {
        if (stream_data(data_fd, sockfd, data_len) == -1) {
            log_msg(ERROR, "Failed to stream file data to editor");
            return -1;
        }
    }

    if (write_all(sockfd, "\n.\n", 3) == -1) {
        log_msg(ERROR, "Failed to send end-of-data to editor: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int
send_stdin(int sockfd, const char* display_name, const char* data, size_t data_len, int line)
{
    return send_open(sockfd, display_name, "-", "-", -1, data, data_len, line);
}

/* A file that does not exist yet is created by its first save, which takes a writable directory. */
int
check_new_file(const char* filename, int force)
{
    char dir_buf[PATH_MAX];
    const char* dir;

    if (strlen(filename) >= sizeof(dir_buf)) {
        log_msg(ERROR, "Failed to stat file %s: %s", filename, strerror(ENAMETOOLONG));
        return -1;
    }
    strcpy(dir_buf, filename);
    dir = dirname(dir_buf);

    if (access(dir, W_OK | X_OK) == -1) {
        if (errno != EACCES && errno != EROFS) {
            log_msg(ERROR, "Failed to access directory %s: %s", dir, strerror(errno));
            return -1;
        }
        if (!force) {
            log_msg(ERROR, "File %s cannot be created! Use -f to open anyway", filename);
            return -1;
        }
        log_msg(WARNING, "File %s cannot be created, opening anyway", filename);
    }

    log_msg(INFO, "%s is a new file", filename);
    return 0;
}

int
check_file(const char* filename, int force)
{
    struct stat st;

    if (strchr(filename, '\n') || strchr(filename, '\r')) {
        log_msg(ERROR, "Filename contains newline or carriage return, refusing to open");
        return -1;
    }

    if (stat(filename, &st) == -1) {
        if (errno != ENOENT) {
            log_msg(ERROR, "Failed to stat file %s: %s", filename, strerror(errno));
            return -1;
        }
        return check_new_file(filename, force);
    }

    if (S_ISDIR(st.st_mode)) {
        log_msg(ERROR, "%s is a directory, aborting", filename);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        log_msg(ERROR, "%s is not a regular file", filename);
        return -1;
    }
    if (access(filename, W_OK) == -1) {
        if (!force) {
            log_msg(ERROR, "File %s is not writable! Use -f to open anyway", filename);
            return -1;
        }
        log_msg(WARNING, "File %s is not writable, opening anyway", filename);
    }

    return 0;
}

/*
 * The absolute path of a file. A file that does not exist yet has none to look
 * up, so its directory is resolved and the file name put back on.
 */
const char*
resolve_path(const char* filename, char* resolved)
{
    char dir_buf[PATH_MAX], base_buf[PATH_MAX], dir_resolved[PATH_MAX];

    if (realpath(filename, resolved)) {
        return resolved;
    }

    if (strlen(filename) < PATH_MAX) {
        strcpy(dir_buf, filename);
        strcpy(base_buf, filename);
        if (realpath(dirname(dir_buf), dir_resolved)) {
            const char* dir = strcmp(dir_resolved, "/") ? dir_resolved : "";
            int len = snprintf(resolved, PATH_MAX, "%s/%s", dir, basename(base_buf));

            if (len > 0 && len < PATH_MAX) {
                return resolved;
            }
        }
    }

    return filename;
}

int
send_file(int sockfd, const char* filename, const char* display_name, int line)
{
    size_t size = 0;
    int fd = open(filename, O_RDONLY | O_CLOEXEC);

    if (fd == -1) {
        /* A file that does not exist yet is sent empty. */
        if (errno != ENOENT) {
            log_msg(ERROR, "Failed to open file %s: %s", filename, strerror(errno));
            return -1;
        }
    } else {
        struct stat st;

        if (fstat(fd, &st) == -1) {
            log_msg(ERROR, "Failed to stat file %s: %s", filename, strerror(errno));
            close(fd);
            return -1;
        }
        size = (size_t)st.st_size;
    }

    char resolved[PATH_MAX];
    const char* rpath = resolve_path(filename, resolved);

    int ret = send_open(sockfd, display_name, rpath, filename, fd, NULL, size, line);
    if (fd != -1) {
        close(fd);
    }
    return ret;
}

int
is_open_file(const struct cmd* cmd_state, const char* token)
{
    for (int i = 0; i < cmd_state->n_files; i++) {
        if (!strcmp(cmd_state->files[i], token)) {
            return 1;
        }
    }
    return 0;
}

int
write_save(const char* filename, const char* data, size_t len)
{
    int fd;

    if (!strcmp(filename, "-")) {
        log_msg(INFO, "Writing %zu bytes to stdout", len);
        fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
    } else {
        log_msg(INFO, "Saving %s (%zu bytes)", filename, len);
        fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    }

    if (fd == -1) {
        log_msg(ERROR, "Failed to open %s for writing: %s", filename, strerror(errno));
        return -1;
    }

    if (write_all(fd, data, len) == -1) {
        log_msg(ERROR, "Failed to write to %s: %s", filename, strerror(errno));
        close(fd);
        return -1;
    }
    if (close(fd) == -1) {
        log_msg(ERROR, "Failed to write to %s: %s", filename, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Takes in the file_size bytes of a save. The first buf_remaining_len of them
 * are already in the protocol read buffer, the rest is read from the socket.
 *
 * The file is opened only after all of the data has arrived. Until then the
 * save can still fall through (connection lost, bogus length) and must not
 * have touched the file.
 *
 * The editor names the file by its token, and any path would do as one. Only
 * the files this session has opened are written.
 *
 * Returns the number of bytes taken from buf_remaining, or -1 if the data
 * could not be read, which leaves the connection unusable. A save that fails
 * later than that is counted in *save_errors and the session carries on.
 */
ssize_t
receive_save(int sockfd,
             const char* buf_remaining,
             size_t buf_remaining_len,
             const struct cmd* cmd_state,
             size_t file_size,
             int* save_errors)
{
    const char* filename = cmd_state->filename;
    size_t from_buf = (buf_remaining_len < file_size) ? buf_remaining_len : file_size;
    char* data = malloc(file_size ? file_size : 1);

    if (!data) {
        log_msg(ERROR, "Failed to allocate %zu bytes for a save: %s", file_size, strerror(errno));
        return -1;
    }

    memcpy(data, buf_remaining, from_buf);
    if (read_all(sockfd, data + from_buf, file_size - from_buf) == -1) {
        free(data);
        return -1;
    }

    if (!filename) {
        log_msg(ERROR, "Received data without token, ignoring");
        (*save_errors)++;
    } else if (!is_open_file(cmd_state, filename)) {
        log_msg(ERROR, "Refusing to save %s: not opened by this session", filename);
        (*save_errors)++;
    } else if (write_save(filename, data, file_size) == -1) {
        (*save_errors)++;
    }

    free(data);
    return (ssize_t)from_buf;
}

/* Accepts a plain decimal number only: no sign, no blanks, nothing after it. */
int
parse_data_len(const char* str, size_t* len)
{
    char* endp;
    unsigned long long val;

    if (*str < '0' || *str > '9') {
        return -1;
    }
    errno = 0;
    val = strtoull(str, &endp, 10);
    if (errno || *endp != '\0' || val > SIZE_MAX) {
        return -1;
    }
    *len = (size_t)val;
    return 0;
}

ssize_t
read_line(char* buf, size_t len)
{
    char* cmd_str = memchr(buf, '\n', len);
    if (!cmd_str) {
        /* No complete line yet; caller should read more data. */
        return 0;
    }

    ssize_t line_len = cmd_str - buf;
    /* Strip \r from \r\n endings. */
    if (line_len > 0 && cmd_str[-1] == '\r') {
        cmd_str[-1] = '\0';
    }
    cmd_str[0] = '\0';

    return line_len + 1;
}

ssize_t
handle_line(int sockfd, char* buf, size_t len, struct cmd* cmd_state, int* save_errors)
{
    ssize_t read_len = 0;
    char *name, *value, *sep;

    switch (cmd_state->state) {
        case CMD_HEADER:
            if ((read_len = read_line(buf, len)) > 0) {
                log_msg(INFO, "Server header: %s", buf);
                cmd_state->state = CMD_CMD;
            }
            break;

        case CMD_CMD:
            if ((read_len = read_line(buf, len)) > 0 && *buf != '\0') {
                free(cmd_state->filename);
                cmd_state->filename = NULL;

                if (!strcmp(buf, "close")) {
                    log_msg(INFO, "Received close command");
                } else if (!strcmp(buf, "save")) {
                    log_msg(INFO, "Received save command");
                }

                cmd_state->state = CMD_VAR;
            }
            break;

        case CMD_VAR:
            if ((read_len = read_line(buf, len)) == 0) {
                /* The line is not complete yet: stay in this state until the rest arrives. */
                break;
            }

            if (*buf == '\0') {
                goto reset_cmd;
            }

            sep = strchr(buf, ':');
            if (!sep) {
                goto reset_cmd;
            }

            name = buf;
            *sep = '\0';
            value = sep + 1;
            while (*value == ' ') {
                value++;
            }

            if (!strcmp(name, "token")) {
                free(cmd_state->filename);
                cmd_state->filename = strdup(value);
                if (!cmd_state->filename) {
                    log_msg(ERROR, "Failed to allocate memory for token: %s", strerror(errno));
                    return -1;
                }
            } else if (!strcmp(name, "data")) {
                size_t data_len;
                ssize_t data_consumed;

                if (parse_data_len(value, &data_len) == -1) {
                    /* Without the length there is no telling where the data ends. */
                    log_msg(ERROR, "Invalid data length: %s", value);
                    return -1;
                }
                data_consumed =
                  receive_save(sockfd, buf + read_len, len - read_len, cmd_state, data_len, save_errors);
                if (data_consumed < 0) {
                    return -1;
                }
                read_len += data_consumed;
            }
            break;

        reset_cmd:
            cmd_state->state = CMD_CMD;
            break;

        default:
            break;
    }

    return read_len;
}

ssize_t
handle_cmds(int sockfd, char* buf, size_t len, struct cmd* cmd_state, int* save_errors)
{
    size_t total_read_len = 0;

    while (total_read_len < len) {
        ssize_t read_len = handle_line(sockfd, buf, len - total_read_len, cmd_state, save_errors);

        if (read_len < 0) {
            return -1;
        }
        if (read_len == 0) {
            break;
        }

        buf += read_len;
        total_read_len += read_len;
    }

    return total_read_len;
}

char*
parse_ssh_host(void)
{
    const char* ssh = getenv("SSH_CONNECTION");
    if (!ssh || !*ssh) {
        return NULL;
    }

    const char* end = strchr(ssh, ' ');
    /* No space means SSH_CONNECTION has only the client IP (unusual). */
    size_t len = end ? (size_t)(end - ssh) : strlen(ssh);
    char* result = malloc(len + 1);
    if (!result) {
        return NULL;
    }
    memcpy(result, ssh, len);
    result[len] = '\0';
    return result;
}

int
connect_mate(const char* host, const char* port)
{
    int sockfd = -1;
    struct addrinfo hints, *servinfo;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rv = getaddrinfo(host, port, &hints, &servinfo);
    if (rv != 0) {
        log_msg(ERROR, "Failed to resolve %s:%s: %s", host, port, gai_strerror(rv));
        return -1;
    }

    for (struct addrinfo* p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue;
        }
        fcntl(sockfd, F_SETFD, FD_CLOEXEC);

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            sockfd = -1;
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if (sockfd != -1) {
        int keepalive = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) == -1) {
            log_msg(WARNING, "Failed to set SO_KEEPALIVE: %s", strerror(errno));
        }
        log_msg(INFO, "Connected to %s:%s", host, port);
    }

    return sockfd;
}

const char*
make_display_name(const char* display_name, const char* hostname, const char* filename, char* buf, size_t buf_len)
{
    if (display_name) {
        return display_name;
    }
    snprintf(buf, buf_len, "%s:%s", hostname, filename);
    return buf;
}

void
version(void)
{
#ifdef BUILD_ID
    const char* build_id = BUILD_ID;
#else
    const char* build_id = "";
#endif
    fprintf(stderr,
            "rmate 2.0.0%s (%s)\n"
            "Copyright (c) 2014 Mael Clerambault\n"
            "Copyright (c) 2026 Jianrong Zhao\n",
            build_id,
            __DATE__);
}

void
usage(void)
{
    fprintf(stderr,
            "Usage: rmate [options] file [file ...]\n"
            "       rmate [options] -      (read from stdin and write the edit to stdout)\n"
            "       rmate [options] - >out (read from stdin and save the edit to file 'out')\n"
            "\n"
            "Options:\n"
            "  -H HOST  Connect to host. Use 'auto' to detect from SSH.\n"
            "           Defaults to $%s or %s.\n"
            "  -p PORT  Port number to use for connection.\n"
            "           Defaults to $%s or %s.\n"
            "  -w       Wait for file to be closed by TextMate.\n"
            "  -l LINE  Place cursor on line number. Repeat for each file,\n"
            "           e.g. -l 5 -l 10 file1 file2 (5 for file1, 10 for file2).\n"
            "  -m NAME  The display name shown in TextMate.\n"
            "  -f       Open even if file is not writable.\n"
            "  -v       Verbose logging messages.\n"
            "  -V       Print version information.\n"
            "  -h       Print this help.\n",
            HOST_ENV,
            DEFAULT_HOST,
            PORT_ENV,
            DEFAULT_PORT);
}

struct options
{
    const char* host;
    const char* port;
    char* auto_host;
    int need_wait;
    int force;
    int* lines;
    int n_lines;
    const char** names;
    int n_names;
};

/*
 * Returns:
 *   0  success, opts populated, *out_argc / *out_argv set to remaining args
 *   1  error (message already logged)
 *  -1  early exit (version/help already printed, caller should exit 0)
 */
int
parse_options(int orig_argc, char* orig_argv[], int* out_argc, char*** out_argv, struct options* opts)
{
    opts->host = getenv(HOST_ENV);
    opts->port = getenv(PORT_ENV);
    if (!opts->host) {
        opts->host = DEFAULT_HOST;
    }
    if (!opts->port) {
        opts->port = DEFAULT_PORT;
    }

    opts->lines = calloc(orig_argc, sizeof(*opts->lines));
    opts->names = calloc(orig_argc, sizeof(*opts->names));
    if (!opts->lines || !opts->names) {
        log_msg(ERROR, "Failed to allocate memory: %s", strerror(errno));
        return 1;
    }

    int ch;
    while ((ch = getopt(orig_argc, orig_argv, "whvfVH:p:l:m:")) != -1) {
        switch (ch) {
            case 'w':
                opts->need_wait = 1;
                break;
            case 'H':
                opts->host = optarg;
                break;
            case 'p':
                opts->port = optarg;
                break;
            case 'l': {
                char* endp;
                errno = 0;
                long val = strtol(optarg, &endp, 10);
                if (errno || endp == optarg || *endp != '\0' || val <= 0 || val > INT_MAX) {
                    log_msg(ERROR, "Invalid line number: %s", optarg);
                    return 1;
                }
                opts->lines[opts->n_lines++] = (int)val;
                break;
            }
            case 'm':
                opts->names[opts->n_names++] = optarg;
                break;
            case 'f':
                opts->force = 1;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'V':
                version();
                return -1;
            case 'h':
            default:
                usage();
                return -1;
        }
    }

    *out_argc = orig_argc - optind;
    *out_argv = orig_argv + optind;

    if (*out_argc < 1) {
        usage();
        return -1;
    }

    if (!strcmp(opts->host, "auto")) {
        opts->auto_host = parse_ssh_host();
        if (opts->auto_host) {
            opts->host = opts->auto_host;
            log_msg(INFO, "Detected host from SSH_CONNECTION: %s", opts->host);
        } else {
            opts->host = DEFAULT_HOST;
            log_msg(INFO, "SSH_CONNECTION not set, falling back to %s", opts->host);
        }
    }

    return 0;
}

void
free_options(struct options* opts)
{
    free(opts->lines);
    free(opts->names);
    free(opts->auto_host);
}

int
send_files(int sockfd,
           int argc,
           char** argv,
           const char* hostname,
           const char* stdin_data,
           size_t stdin_len,
           const struct options* opts)
{
    char dname_buf[HOST_NAME_MAX + 1 + PATH_MAX + 1];

    for (int i = 0; i < argc; i++) {
        const char* per_name = (i < opts->n_names) ? opts->names[i] : NULL;
        int per_line = (i < opts->n_lines) ? opts->lines[i] : 0;
        const char* dname;

        if (!strcmp(argv[i], "-")) {
            dname = make_display_name(per_name, hostname, "untitled (stdin)", dname_buf, sizeof(dname_buf));
            if (send_stdin(sockfd, dname, stdin_data, stdin_len, per_line) == -1) {
                return -1;
            }
        } else {
            dname = make_display_name(per_name, hostname, argv[i], dname_buf, sizeof(dname_buf));
            if (send_file(sockfd, argv[i], dname, per_line) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

int
receive_loop(int sockfd, int n_files, char* const* files, int* save_errors)
{
    char buf[PROTO_BUF_SIZE];
    size_t buf_used = 0;
    struct cmd cmd_state = { .files = files, .n_files = n_files };
    int ret = 0;

    while (1) {
        if (buf_used >= sizeof(buf) - 1) {
            log_msg(ERROR, "Protocol line too long");
            ret = 1;
            break;
        }

        ssize_t num_bytes = read(sockfd, buf + buf_used, sizeof(buf) - 1 - buf_used);
        if (num_bytes == -1) {
            if (errno == EINTR) {
                continue;
            }
            log_msg(ERROR, "Failed to read from server: %s", strerror(errno));
            ret = 1;
            break;
        }
        if (num_bytes == 0) {
            break;
        }

        buf_used += num_bytes;
        buf[buf_used] = '\0';

        ssize_t consumed = handle_cmds(sockfd, buf, buf_used, &cmd_state, save_errors);
        if (consumed < 0) {
            ret = 1;
            break;
        }
        if (consumed > 0) {
            if ((size_t)consumed < buf_used) {
                memmove(buf, buf + consumed, buf_used - consumed);
                buf_used -= consumed;
            } else {
                buf_used = 0;
            }
        }
    }

    if (*save_errors > 0) {
        log_msg(WARNING, "%d save(s) failed", *save_errors);
        ret = 1;
    }

    free(cmd_state.filename);
    return ret;
}

void
close_connection(int sockfd)
{
    /* Send FIN so the server gets a clean EOF immediately. */
    shutdown(sockfd, SHUT_WR);
    /*
     * Drain unread data (e.g. server's protocol header) before close.
     * On macOS/BSD, close() on a socket with unread data sends RST
     * instead of FIN, causing "Connection reset by peer" on the server.
     */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char drain[BUFSIZ];
    while (read(sockfd, drain, sizeof(drain)) > 0)
        ;
    close(sockfd);
}

/*
 * Whoever started rmate without a terminal, as in "ssh host rmate file", waits
 * for its stdout and stderr to be closed, not just for the foreground process
 * to exit. So the background process lets go of them.
 *
 * stdout stays if a save goes there (stdin mode). stderr stays if it is a
 * terminal: that keeps nobody waiting, and it is where errors get read.
 */
void
release_stdio(int keep_stdout)
{
    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

    if (null_fd == -1) {
        return;
    }
    dup2(null_fd, STDIN_FILENO);
    if (!keep_stdout) {
        dup2(null_fd, STDOUT_FILENO);
    }
    if (!isatty(STDERR_FILENO)) {
        dup2(null_fd, STDERR_FILENO);
    }
    if (null_fd > STDERR_FILENO) {
        close(null_fd);
    }
}

int
main(int argc, char* argv[])
{
    int sockfd = -1;
    int ret = 0;
    struct options opts = { 0 };
    char hostname[HOST_NAME_MAX + 1];
    char* stdin_data = NULL;
    size_t stdin_len = 0;
    int stdin_mode = 0;
    int save_errors = 0;

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    int pr = parse_options(argc, argv, &argc, &argv, &opts);
    if (pr != 0) {
        ret = (pr > 0) ? 1 : 0;
        goto cleanup;
    }

    if (gethostname(hostname, sizeof(hostname)) == -1) {
        snprintf(hostname, sizeof(hostname), "unknown");
    }
    hostname[sizeof(hostname) - 1] = '\0';

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-")) {
            stdin_mode = 1;
            /* Only read stdin once; multiple "-" args reuse the same data. */
            if (!stdin_data) {
                if (isatty(STDIN_FILENO)) {
                    fprintf(stderr, "Reading from stdin, press ^D to stop\n");
                } else {
                    log_msg(INFO, "Reading from stdin");
                }
                stdin_data = read_stdin(&stdin_len);
                if (!stdin_data) {
                    log_msg(ERROR, "Failed to read from stdin: %s", strerror(errno));
                    ret = 1;
                    goto cleanup;
                }
            }
        } else if (check_file(argv[i], opts.force) == -1) {
            ret = 1;
            goto cleanup;
        }
    }

    if ((sockfd = connect_mate(opts.host, opts.port)) == -1) {
        log_msg(ERROR, "Could not connect to %s:%s", opts.host, opts.port);
        ret = 1;
        goto cleanup;
    }

    if (send_files(sockfd, argc, argv, hostname, stdin_data, stdin_len, &opts) == -1) {
        ret = 1;
        goto cleanup;
    }

    free(stdin_data);
    stdin_data = NULL;

    if (!opts.need_wait) {
        pid_t pid = fork();
        if (pid == -1) {
            log_msg(ERROR, "Failed to fork background process: %s", strerror(errno));
            ret = 1;
            goto cleanup;
        }
        if (pid > 0) {
            _exit(0);
        }
        /* Detach from the controlling terminal so SIGHUP/SIGINT won't reach us. */
        setsid();
        release_stdio(stdin_mode);
    }

    ret = receive_loop(sockfd, argc, argv, &save_errors);

cleanup:
    if (sockfd != -1) {
        close_connection(sockfd);
    }
    free(stdin_data);
    free_options(&opts);
    log_msg(INFO, "Done");

    return ret;
}
