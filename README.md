# rmate.c

A lightweight C implementation of [rmate](https://github.com/textmate/rmate),
the remote editing client for TextMate, VS Code (via
[Remote VSCode](https://marketplace.visualstudio.com/items?itemName=rafaelmaiolla.remote-vscode)),
Sublime Text (via
[RemoteSubl](https://github.com/randy3k/RemoteSubl)), and other editors that speak the rmate protocol.

`rmate` lets you open files on a remote server in your local editor over an SSH
tunnel. The C version is a drop-in replacement for the
[Bash](https://github.com/aurora/rmate),
[Python](https://github.com/sclukey/rmate-python), and
[Ruby](https://github.com/textmate/rmate) versions, with significantly faster
startup, lower memory usage, and zero runtime dependencies.

This is a fork of [silverzhaojr/rmate.c](https://github.com/silverzhaojr/rmate.c),
the rewrite of the original [hanklords/rmate.c](https://github.com/hanklords/rmate.c).
It fixes several ways in which a save could damage a file and adds a test
suite. [History](#history) has the details.

## Quick Start

```sh
# Build
make

# Install (default: /usr/bin)
sudo make install

# Open a file
rmate myfile.txt

# Read from stdin, edit, write result to stdout
echo "hello" | rmate -w -
```

The editor must be listening on the rmate port (default 52698). For SSH usage,
set up a reverse tunnel:

```sh
ssh -R 52698:localhost:52698 user@remote
```

Or add to `~/.ssh/config`:

```
Host myserver
  RemoteForward 52698 localhost:52698
```

## Usage

```
Usage: rmate [options] file [file ...]
       rmate [options] -      (read from stdin and write the edit to stdout)
       rmate [options] - >out (read from stdin and save the edit to file 'out')

Options:
  -H HOST  Connect to host. Use 'auto' to detect from SSH.
           Defaults to $RMATE_HOST or localhost.
  -p PORT  Port number to use for connection.
           Defaults to $RMATE_PORT or 52698.
  -w       Wait for file to be closed by TextMate.
  -l LINE  Place cursor on line number. Repeat for each file,
           e.g. -l 5 -l 10 file1 file2 (5 for file1, 10 for file2).
  -m NAME  The display name shown in TextMate.
  -f       Open even if file is not writable.
  -v       Verbose logging messages.
  -V       Print version information.
  -h       Print this help.
```

## Environment Variables

| Variable     | Description                          | Default     |
|--------------|--------------------------------------|-------------|
| `RMATE_HOST` | Editor host to connect to            | `localhost` |
| `RMATE_PORT` | Editor port to connect to            | `52698`     |

Setting `RMATE_HOST=auto` detects the client IP from `SSH_CONNECTION`.

## Exit Status

`rmate` exits with 0 on success and with 1 if something went wrong: a mistake
on the command line, a file that cannot be opened, an editor that cannot be
reached, a broken connection, or a save that failed.

Without `-w` the exit status only covers what happens before `rmate` goes
into the background, which is everything up to and including sending the
files to the editor. What goes wrong later is reported on the terminal. A
background process started without a terminal, as in `ssh host rmate file`,
reports to syslog instead, as `rmate`.

## Security

The editor's port is a plain TCP port on `localhost`. With the SSH tunnel up
it leads to your editor. Without it, any user of the machine can listen
there, and `rmate` cannot tell the difference.

Whoever answers on the port is sent the files you open, and can save them.
That is as far as it goes: `rmate` writes a save only to a file it has opened
in the same session, and refuses any other path.

## History

### 2.1.0

Most of these are fixes for ways in which a save could go wrong. Each comes
with a test.

- **A save is complete before the file is touched.** 2.0.0 truncated the file
  first and then filled it from the network, so a connection lost mid-save
  left a truncated file, and so did a malformed length. The new content is
  now held in memory until all of it has arrived.
- **A save is only accepted for a file that was opened.** 2.0.0 wrote to
  whatever path the editor named and created it if it did not exist. See
  [Security](#security).
- **A header line split across two packets no longer drops the save.**
- **A malformed save cannot derail the session.** The data of a save without
  token is skipped, where 2.0.0 parsed it as commands. A length that is not a
  number ends the session, since the end of the data cannot be found.
- **A file that does not exist yet is created by its first save**, with a
  mode that follows the umask. 2.0.0 created it when it was opened, which
  left an empty file behind if it was closed without saving.
- **The background process lets go of stdin, stdout and stderr**, so
  `ssh host rmate file` returns at once. 2.0.0 kept the caller waiting until
  the editor closed the file. Without a terminal, errors go to syslog.
- **`-h` and `-V` print to stdout, and a mistake on the command line exits
  with status 1.** 2.0.0 printed to stderr and exited with 0 in all three
  cases.
- **The build honours `CFLAGS`, `CPPFLAGS` and `LDFLAGS`**, and the version
  output no longer contains the day of the build.
- **There is a test suite**: `make check`, see [Testing](#testing).

### 2.0.0: the Rewrite

Version 2.0.0 is a rewrite of the original
[rmate.c by Mael Clerambault](https://github.com/hanklords/rmate.c) by
Jianrong Zhao, who wrote about it:

> The code changes are made with the help of [Cursor CLI](https://cursor.com/cli),
> but I spent a lot of time reviewing and revising them very carefully. So don't
> panic, it's not just trivial AI generated code :-).

The following is a summary of its changes compared to the original.

#### New Features

- **Stdin support (`-`)**: Read from stdin, send to the editor, and write the
  edited result back to stdout. The original only supported named files.
- **Multiple files**: Open several files in a single invocation
  (`rmate a.txt b.txt`). The original was limited to one file.
- **Per-file line selection (`-l`)**: Jump to a specific line number in each
  file. Supports multiple `-l` flags, one per file.
- **Per-file display names (`-m`)**: Override the tab name shown in the editor
  for each file.
- **Force open (`-f`)**: Open read-only files with a warning instead of
  refusing.
- **Verbose logging (`-v`)**: Detailed protocol-level logging to stderr for
  debugging connection issues.
- **Auto host detection (`-H auto`)**: Automatically extract the client IP
  from the `SSH_CONNECTION` environment variable.

#### Robustness Improvements

- **Complete rewrite of I/O handling**: All `read()`/`write()` calls handle
  `EINTR`, partial reads, and partial writes correctly. The original used bare
  `read()`/`write()` without retry.
- **Streaming file send**: Files are streamed from the file descriptor directly
  to the socket in chunks (`stream_data`), instead of being `mmap()`'d into
  memory. This keeps RSS constant regardless of file size.
- **Proper protocol buffer management**: The receive loop uses `memmove()` to
  shift unconsumed bytes to the front of the buffer, correctly handling TCP
  fragmentation where a single `read()` may return partial lines. The original
  assumed each `read()` returned aligned protocol messages.
- **Save-path error resilience**: The data of a save is received completely
  before the file is opened, so a connection that drops mid-save leaves the
  file as it was. If writing to disk then fails, the protocol stream is still
  in sync. Save failures are counted and reflected in the exit code without
  aborting saves for other files.
- **Clean socket shutdown**: Uses `shutdown(SHUT_WR)` followed by a timed
  drain before `close()`, preventing RST packets on macOS/BSD.
- **Proper background detach**: Uses `fork()` + `setsid()` to fully detach
  from the controlling terminal, preventing `SIGHUP`/`SIGINT` from killing
  the background save handler.
- **SIGPIPE handling**: Ignores `SIGPIPE` so a broken editor connection
  produces a clean error message instead of a silent crash.
- **Filename validation**: Rejects filenames containing `\n` or `\r` to
  prevent protocol injection.
- **File descriptor hygiene**: All file descriptors are opened with
  `O_CLOEXEC` (or equivalent) to prevent leaks across `fork()`.
- **Memory safety**: All allocations are checked, all paths go through
  `goto cleanup` for deterministic resource release, and `size_t` overflow
  is detected in the stdin buffer growth.

## Performance

See [BENCHMARK.md](BENCHMARK.md) for detailed measurements. Summary:

| Metric | C | Bash | Python | Ruby |
|--------|---|------|--------|------|
| Startup | **0.3 ms** | 2.2 ms | 34 ms | 44 ms |
| Memory (file mode) | **1.7 MB constant** | 3.7 MB constant | Scales with file | Scales with file |
| Memory (100 MB stdin) | **104 MB** | 496 MB | 215 MB | 120 MB |

The memory figures are for sending a file to the editor. A save coming back
is held in memory until it is complete, so it needs as much as the file is
large.

## Testing

```sh
make check
```

builds `rmate` and runs the tests in `tests/test_rmate.py` against it. They need
Python 3.9 or later and nothing else: a fake editor in the test file plays the editor side
of the protocol the way RemoteSubl does, so no editor and no SSH tunnel are
involved.

## Protocol

See [PROTOCOL.md](PROTOCOL.md) for a description of the rmate wire protocol.

## Implementation

See [IMPLEMENTATION.md](IMPLEMENTATION.md) for code walkthrough and design
details.

## License

ISC, see [LICENSE](LICENSE). Based on
[rmate.c](https://github.com/hanklords/rmate.c) by Mael Clerambault, rewritten
by Jianrong Zhao.
