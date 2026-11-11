# Implementation Details

This document describes the internal structure of `rmate.c` to help new
contributors understand the code.

## File Layout

The source is a single `rmate.c` file with no external dependencies beyond
POSIX libc. Functions appear in bottom-up order: utilities first, then
protocol helpers, then option parsing and high-level orchestration, then
`main()`.

## Core Data Structures

### `struct cmd`

```c
struct cmd {
    enum CMD_STATE state;   /* CMD_HEADER, CMD_CMD, or CMD_VAR */
    char* filename;         /* token from the server (heap-allocated) */
    char* const* files;     /* tokens of the files this session has opened */
    int n_files;
};
```

This is the state machine context for parsing server responses. A single
instance is reused across all commands on one connection. `files` points
at the file arguments of the command line, which are the tokens sent in the
`open` commands.

### `enum CMD_STATE`

```
CMD_HEADER --(greeting)--> CMD_CMD --(command)--> CMD_VAR
                              ^                      |
                              |                      |
                              +--(empty line)--------+
```

- **CMD_HEADER**: Initial state. Consumes the server's greeting line
  (e.g. `mate-server 1.0`), then transitions to CMD_CMD.
- **CMD_CMD**: Waits for a command keyword (`save` or `close`).
- **CMD_VAR**: Parses `key: value` pairs. When `data:` is encountered, it
  triggers `receive_save()` to consume the file bytes. An empty line resets
  back to CMD_CMD.

## Protocol: Client → Server

### send\_open()

The central function for sending a file to the editor:

```c
int send_open(int sockfd,
              const char* display_name,
              const char* real_path,
              const char* token,
              int data_fd,          /* file descriptor, or -1 */
              const char* data_buf, /* in-memory buffer, or NULL */
              size_t data_len,
              int line);
```

It builds the full protocol header in a stack buffer via `snprintf()`, sends
it with `write_all()`, then sends the file content using one of two paths:

1. **`data_buf != NULL`** (stdin mode): The data is already in memory; write
   it directly.
2. **`data_fd >= 0`** (file mode): Stream from file descriptor to socket via
   `stream_data()`, reading and writing in `BUFSIZ` chunks. The file is never
   loaded into memory.

Finally, `"\n.\n"` terminates the command.

Two convenience wrappers call `send_open()`:

- **`send_file()`**: Opens the file, `fstat()`s it for the size, resolves the
  real path, and calls `send_open()` with the file descriptor. A file that
  does not exist yet is sent with no data; `resolve_path()` gives it a real
  path by resolving its directory.
- **`send_stdin()`**: Calls `send_open()` with the pre-read stdin buffer,
  using `"-"` for both real-path and token.

## Protocol: Server → Client

### Buffer Management

TCP is a byte stream — a single `read()` may return a partial line, one
complete line, or multiple lines concatenated. The receive loop in
`receive_loop()` handles this:

```
+------------------------------------------+
|           buf[PROTO_BUF_SIZE]            |
|  [consumed data][unconsumed][  free  ]   |
|                  ^buf_used               |
+------------------------------------------+
```

1. `read()` appends new data after `buf_used`.
2. `handle_cmds()` processes as many complete `\n`-delimited lines as
   possible, returning the total bytes consumed.
3. If `consumed < buf_used`, `memmove()` shifts the leftover tail to the
   front. (`memmove` is required because source and destination overlap.)
4. The next `read()` appends after the leftover.

### read\_line()

Scans for `\n` in the buffer. If found, NUL-terminates the line (stripping
`\r\n` endings), and returns the number of bytes consumed (including the
newline). Returns 0 if no complete line is available yet.

### handle\_line()

A dispatch function driven by `cmd_state->state`:

- **CMD_HEADER**: Read one line (server greeting), transition to CMD_CMD.
- **CMD_CMD**: Read a command name. Reset state, then transition to CMD_VAR.
- **CMD_VAR**: Parse `key: value`. On `token:`, save the filename. On
  `data:`, call `receive_save()`. On empty line or parse error, transition
  back to CMD_CMD. A `data:` value that is not a plain decimal number ends
  the session: without the length there is no telling where the data ends.

### receive\_save()

Takes in the data of a save and writes it to disk:

```c
ssize_t receive_save(int sockfd,
                     const char* buf_remaining,
                     size_t buf_remaining_len,
                     const struct cmd* cmd_state,
                     size_t file_size,
                     int* save_errors);
```

When `handle_line()` encounters `data: N`, some of the N file bytes may
already be in the protocol buffer (read in the same TCP segment as the
`data:` header). `receive_save()` handles this in three steps:

1. Copy `buf_remaining` (the bytes already in the buffer) into a heap buffer
   of N bytes.
2. Call `read_all()` to read the remaining bytes from the socket into that
   buffer.
3. Call `write_save()` to open the file and write the buffer to it.

The file is not opened before all N bytes have arrived. Until then the save
can still fall through — the connection drops, or N is nonsense — and
opening the file means truncating it. The price is memory: a save needs as
much of it as the file is large. (Sending a file to the editor is streamed
and needs none.)

The token names the file to write, and the editor could put any path there.
`receive_save()` therefore writes only if the token is one of
`cmd_state->files`, the files this session has opened. Whoever answers on the
editor's port — it is a TCP port on localhost, open to every user of the
machine while the SSH tunnel is down — can then change the files handed to
it, but no others.

If the token is `"-"`, `write_save()` writes to stdout instead of a file,
enabling `echo "text" | rmate -w - > edited.txt`.

**Save error tracking**: A single rmate connection handles saves for all open
files. If `receive_save()` returned a fatal error on one file's save failure,
the receive loop would exit and subsequent saves for other files would never
be processed. To avoid this, a save that fails after its data has arrived (no
token, a token that was not opened, open error, write error) increments the
caller's `*save_errors` counter instead of returning -1. The data has been consumed, so the protocol
is still in sync and the loop continues. After the loop exits, the counter is
checked and the process exit code is set to 1 if any saves failed.

Only a failure to get the data itself (allocation, read error, connection
closed) returns -1 and ends the session.

## I/O Helpers

### write\_all()

Retries `write()` in a loop until all bytes are sent, handling `EINTR` and
short writes. Returns 0 on success, -1 on error.

### read\_all()

Retries `read()` in a loop until exactly the requested number of bytes has
arrived, handling `EINTR` and short reads. Returns 0 on success, -1 on error
or if the other side closes the connection first.

### stream\_data()

Transfers `len` bytes from `from_fd` to `to_fd` in `BUFSIZ` chunks:

```c
int stream_data(int from_fd, int to_fd, size_t len);
```

- Handles `EINTR` on reads.
- Detects unexpected EOF.
- Returns 0 on success, -1 on failure.

### read\_stdin()

Reads all of stdin into a `malloc()`'d buffer using exponential growth
(starting at 4 KB, doubling each time). Detects `size_t` overflow from
doubling. Returns `NULL` on error with `errno` set.

## Option Parsing and Main Structure

### `struct options`

```c
struct options {
    const char* host;
    const char* port;
    char* auto_host;    /* owned, freed by free_options() */
    int need_wait;
    int force;
    int* lines;         /* owned, freed by free_options() */
    int n_lines;
    const char** names; /* owned, elements point into argv */
    int n_names;
};
```

Groups all command-line configuration. Passed by pointer to functions that
need it.

### parse\_options()

Handles `getopt()`, validates `-l` line numbers with `strtol()`, resolves
`-H auto` via `parse_ssh_host()`, and adjusts `argc`/`argv` past the options.
Returns 0 on success, 1 on error, or -1 for early exit (`-V`/`-h`).

### send\_files()

Iterates the positional arguments, builds display names, and calls
`send_file()` or `send_stdin()` for each. Extracted from `main()` to keep it
focused on orchestration.

### receive\_loop()

Owns the protocol buffer and `struct cmd` state machine. Reads from the
server socket, dispatches via `handle_cmds()`, and tracks `save_errors`.
Returns 0 on clean shutdown or 1 on error.

### close\_connection()

Performs graceful socket shutdown: sends FIN via `shutdown(SHUT_WR)`, drains
unread data with a short timeout, then closes the socket.

### check\_file()

Validates a filename before opening: rejects names containing `\n` or `\r`
(to prevent protocol injection), checks that the path is a regular file,
and verifies write permission (unless `-f` is set).

A file that does not exist is not created here. Its first save does that, so
opening a mistyped name and closing it again leaves nothing behind.
`check_new_file()` only makes sure the directory exists and is writable
(unless `-f` is set).

## Connection Lifecycle

### connect\_mate()

Uses `getaddrinfo()` to resolve the host/port (supporting both IPv4 and
IPv6), then tries each address in order. On success, enables `SO_KEEPALIVE`
to detect dead connections.

### Background Mode (default)

After sending all files, `main()` forks. The parent exits immediately
(returning the shell prompt), and the child calls `setsid()` to detach from
the controlling terminal before entering the receive loop. This prevents
`SIGHUP` or `SIGINT` from killing the background process.

### Wait Mode (`-w`)

The process stays in the foreground and blocks in the receive loop until the
server closes the connection (all editor tabs closed).

### Clean Shutdown

When the receive loop ends:

1. `shutdown(sockfd, SHUT_WR)` sends a TCP FIN.
2. A short timed drain loop reads any remaining data from the socket.
3. `close(sockfd)` releases the socket.

Step 2 prevents a TCP RST on macOS/BSD, where closing a socket with unread
data in the receive buffer causes the kernel to send RST instead of FIN.

## Signal Handling

- **`SIGCHLD`**: Set to `SIG_IGN` so forked children are automatically
  reaped (no zombies).
- **`SIGPIPE`**: Set to `SIG_IGN` so writing to a broken socket returns
  `EPIPE` instead of killing the process with a signal. This lets the error
  handling code produce a meaningful message.

## File Descriptor Hygiene

All `open()` calls use `O_CLOEXEC`, `dup()` uses `F_DUPFD_CLOEXEC`, and
`socket()` is followed by `fcntl(fd, F_SETFD, FD_CLOEXEC)`. This prevents
file descriptor leaks across `fork()`/`exec()` boundaries.

## Error Handling Strategy

All functions return -1 on failure. `main()` uses a single `goto cleanup`
pattern:

```c
int main(...) {
    int ret = 0;
    ...
    if (something_failed) {
        ret = 1;
        goto cleanup;
    }
    ...
cleanup:
    close_connection(...);
    free_options(...);
    return ret;
}
```

This ensures deterministic cleanup of all resources (file descriptors, heap
allocations, socket) on every exit path.

`parse_options()` uses a three-way return (0 / 1 / -1) so `main()` can
distinguish success, error, and early exit (help/version).

Save failures during the receive loop are non-fatal: `receive_save()`
increments a `save_errors` counter, and since it has consumed the data by
then, the protocol stays in sync. The exit code is set to 1 after the loop
if any saves failed.
