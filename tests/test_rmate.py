#!/usr/bin/env python3
"""Tests for rmate, run against a fake editor.

The fake editor plays the editor side of the rmate protocol the way the
RemoteSubl plugin for Sublime Text does: it greets, parses the "open"
commands rmate sends, and answers with "save" and "close" commands.

Run with `make check`, or directly:

    RMATE=./rmate python3 tests/test_rmate.py [-v] [TestClass[.test_name]]
"""

import collections
import os
import select
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest

RMATE = os.path.abspath(os.environ.get("RMATE", os.path.join(os.path.dirname(__file__), "..", "rmate")))
TIMEOUT = 10  # seconds; generous, nothing here should take longer than a moment

# How a save reaches rmate. TCP has no message boundaries, so all of these must work.
ONE_SEGMENT = "one segment"    # header and payload in a single write
LIKE_PLUGIN = "like plugin"    # four writes: "save", "token", "data", payload (what RemoteSubl does)
LINE_BY_LINE = "line by line"  # every header line arrives on its own

DELIVERIES = (ONE_SEGMENT, LIKE_PLUGIN, LINE_BY_LINE)

Result = collections.namedtuple("Result", "status stdout stderr opens")


class FakeEditor:
    """Accepts one connection from rmate and plays the editor.

    Each "open" command received ends up in `opens` as a dict of its headers,
    with the file content under the key "content".
    """

    def __init__(self):
        self._listener = socket.socket()
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self._listener.settimeout(0.05)
        self.port = self._listener.getsockname()[1]
        self.opens = []
        self._conn = None
        self._done = False  # rmate closed its side, or never connected
        self._stop_accepting = threading.Event()
        self._changed = threading.Condition()
        self._reader = threading.Thread(target=self._read_commands, daemon=True)
        self._reader.start()

    def _accept(self):
        while not self._stop_accepting.is_set():
            try:
                return self._listener.accept()[0]
            except socket.timeout:
                continue
            except OSError:
                break
        return None

    def _read_commands(self):
        conn = self._accept()
        if conn is None:
            self._finish()
            return
        conn.settimeout(None)
        conn.sendall(b"Sublime Text 3 (remote_subl plugin)\n")
        with self._changed:
            self._conn = conn
        stream = conn.makefile("rb")
        current = None
        try:
            for line in iter(stream.readline, b""):
                if current is None:
                    if line.strip() == b"open":
                        current = {}
                    continue
                text = line.decode("utf-8", "replace").strip()
                if ":" not in text:
                    continue
                key, value = (part.strip() for part in text.split(":", 1))
                current[key] = value
                if key == "data":
                    current["content"] = stream.read(int(value)) if int(value) else b""
                    with self._changed:
                        self.opens.append(current)
                        self._changed.notify_all()
                    current = None
        except OSError:
            pass
        self._finish()

    def _finish(self):
        with self._changed:
            self._done = True
            self._changed.notify_all()

    def give_up(self):
        """Stop waiting for a connection or for more "open" commands."""
        self._stop_accepting.set()
        self._finish()

    def wait_for_opens(self, count):
        """Wait until `count` files were opened; returns False if that never happens."""
        deadline = time.time() + TIMEOUT
        with self._changed:
            while len(self.opens) < count and not self._done and time.time() < deadline:
                self._changed.wait(deadline - time.time())
            return len(self.opens) >= count

    def send(self, *chunks, gap=0.0):
        """Send raw bytes, one write per chunk."""
        for chunk in chunks:
            try:
                self._conn.sendall(chunk)
            except OSError:
                return
            if gap:
                time.sleep(gap)

    def save(self, token, content, delivery=LIKE_PLUGIN):
        header = [b"save\n", b"token: " + token.encode() + b"\n", b"data: %d\n" % len(content)]
        if delivery == ONE_SEGMENT:
            self.send(b"".join(header) + content + b"\n")
        elif delivery == LIKE_PLUGIN:
            self.send(*header, content + b"\n")
        elif delivery == LINE_BY_LINE:
            self.send(*header, gap=0.05)
            self.send(content + b"\n")
        else:
            raise ValueError(delivery)

    def close_file(self, token):
        self.send(b"close\n", b"token: " + token.encode() + b"\n", b"\n")

    def hang_up(self):
        """Close the connection the polite way: FIN first, then wait for rmate to close its side."""
        self._stop_accepting.set()
        if self._conn:
            try:
                self._conn.shutdown(socket.SHUT_WR)
            except OSError:
                pass
        self._reader.join(TIMEOUT)
        self._listener.close()
        if self._conn:
            self._conn.close()


class RmateTestCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="rmate-test-")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def path(self, name):
        return os.path.join(self.dir, name)

    def write(self, name, content):
        with open(self.path(name), "wb") as f:
            f.write(content)
        return self.path(name)

    def read(self, name):
        try:
            with open(self.path(name), "rb") as f:
                return f.read()
        except FileNotFoundError:
            return None

    def edit(self, files, editor_script=None, options=(), stdin=None):
        """Run `rmate -w` on `files` (relative to the test directory).

        Once all files have been opened, `editor_script(editor)` plays the
        editor; afterwards the editor hangs up, which ends rmate.
        """
        editor = FakeEditor()

        def play():
            if editor.wait_for_opens(len(files)) and editor_script:
                editor_script(editor)
            editor.hang_up()

        player = threading.Thread(target=play, daemon=True)
        player.start()
        command = [RMATE, "-w", "-p", str(editor.port)] + list(options) + list(files)
        done = subprocess.run(command, cwd=self.dir, input=stdin, timeout=TIMEOUT,
                              stdin=None if stdin is not None else subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        editor.give_up()  # rmate is gone; stop waiting if it never opened anything
        player.join(TIMEOUT)
        return Result(done.returncode, done.stdout, done.stderr.decode("utf-8", "replace"), editor.opens)


def lines_of_text(size, word="text"):
    line = ("%s 0123456789 abcdefghijklmnopqrstuvwxyz\n" % word).encode()
    return (line * (size // len(line) + 1))[:size]


class OpenTest(RmateTestCase):
    def test_sends_content_and_headers(self):
        path = self.write("file.txt", b"hello\nworld\n")
        result = self.edit(["file.txt"])
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual(len(result.opens), 1)
        opened = result.opens[0]
        self.assertEqual(opened["content"], b"hello\nworld\n")
        self.assertEqual(opened["token"], "file.txt")
        self.assertEqual(opened["real-path"], os.path.realpath(path))
        self.assertEqual(opened["display-name"], socket.gethostname() + ":file.txt")
        self.assertEqual(opened["data-on-save"], "yes")
        self.assertNotIn("selection", opened)

    def test_empty_file(self):
        self.write("empty.txt", b"")
        result = self.edit(["empty.txt"])
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual([o["content"] for o in result.opens], [b""])

    def test_large_file(self):
        content = os.urandom(1 << 16) * 64  # 4 MiB
        self.write("large.bin", content)
        result = self.edit(["large.bin"])
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual(result.opens[0]["content"], content)

    def test_binary_content_without_trailing_newline(self):
        content = b"\x00\x01\xff line\r\nsecond\r\nno newline at the end"
        self.write("data.bin", content)
        result = self.edit(["data.bin"])
        self.assertEqual(result.opens[0]["content"], content)

    def test_several_files(self):
        self.write("a.txt", b"a\n")
        self.write("b.txt", b"b\n")
        result = self.edit(["a.txt", "b.txt"])
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual([(o["token"], o["content"]) for o in result.opens], [("a.txt", b"a\n"), ("b.txt", b"b\n")])

    def test_line_and_display_name_options(self):
        self.write("file.txt", b"x\n")
        result = self.edit(["file.txt"], options=["-l", "42", "-m", "custom name"])
        self.assertEqual(result.opens[0]["selection"], "42")
        self.assertEqual(result.opens[0]["display-name"], "custom name")

    def test_filename_with_space_colon_and_umlaut(self):
        name = "mein file: ü.txt"
        self.write(name, b"alt\n")
        result = self.edit([name], lambda editor: editor.save(name, b"neu\n"))
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual(result.opens[0]["token"], name)
        self.assertEqual(self.read(name), b"neu\n")

    def test_new_file_is_created_by_save(self):
        result = self.edit(["new.txt"], lambda editor: editor.save("new.txt", b"hello\n"))
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual([o["content"] for o in result.opens], [b""])
        self.assertEqual(self.read("new.txt"), b"hello\n")

    @unittest.expectedFailure  # the file is created as soon as it is opened
    def test_new_file_is_not_created_without_save(self):
        result = self.edit(["typo.txt"], lambda editor: editor.close_file("typo.txt"))
        self.assertEqual(len(result.opens), 1)
        self.assertIsNone(self.read("typo.txt"))

    @unittest.skipIf(os.geteuid() == 0, "root may write to read-only files")
    def test_read_only_file_is_refused(self):
        os.chmod(self.write("ro.txt", b"x\n"), 0o444)
        result = self.edit(["ro.txt"])
        self.assertNotEqual(result.status, 0)
        self.assertEqual(result.opens, [])
        self.assertIn("not writable", result.stderr)

    def test_read_only_file_opens_with_force(self):
        os.chmod(self.write("ro.txt", b"x\n"), 0o444)
        result = self.edit(["ro.txt"], options=["-f"])
        self.assertEqual([o["content"] for o in result.opens], [b"x\n"])

    def test_directory_is_refused(self):
        os.mkdir(self.path("dir"))
        result = self.edit(["dir"])
        self.assertNotEqual(result.status, 0)
        self.assertEqual(result.opens, [])
        self.assertIn("directory", result.stderr)

    def test_stdin_round_trip(self):
        result = self.edit(["-"], lambda editor: editor.save("-", b"edited\n"), stdin=b"from stdin\n")
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual(result.opens[0]["token"], "-")
        self.assertEqual(result.opens[0]["content"], b"from stdin\n")
        self.assertEqual(result.stdout, b"edited\n")


class SaveTest(RmateTestCase):
    def save_and_check(self, new_content, delivery, old_content=b"old content\n"):
        self.write("file.txt", old_content)
        result = self.edit(["file.txt"], lambda editor: (editor.save("file.txt", new_content, delivery),
                                                         editor.close_file("file.txt")))
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual(self.read("file.txt"), new_content)

    def test_small_file(self):
        for delivery in DELIVERIES:
            with self.subTest(delivery=delivery):
                self.save_and_check(lines_of_text(300), delivery)

    def test_large_file(self):
        for delivery in DELIVERIES:
            with self.subTest(delivery=delivery):
                self.save_and_check(lines_of_text(200000), delivery)

    def test_shrinking_and_emptying_a_file(self):
        self.save_and_check(b"x\n", ONE_SEGMENT, old_content=lines_of_text(5000))
        self.save_and_check(b"", ONE_SEGMENT, old_content=lines_of_text(5000))

    def test_content_that_looks_like_protocol(self):
        yaml = b"apiVersion: v1\nkind: ConfigMap\ndata:\n  key: value\n"
        hostile = b"\xfe\x00data: 5\nsave\ntoken: /etc/passwd\n\nclose\n\x00tail"
        for content in (yaml, hostile):
            for delivery in DELIVERIES:
                with self.subTest(content=content[:12], delivery=delivery):
                    self.save_and_check(content, delivery)

    def test_payload_sizes_around_the_buffer_edge(self):
        # The receive buffer holds PATH_MAX + 256 bytes: 1280 on macOS, 4352 on Linux.
        for edge in (1280, 4352):
            for size in range(edge - 40, edge + 4):
                with self.subTest(size=size):
                    self.save_and_check(lines_of_text(size, "s%d" % size), ONE_SEGMENT)

    def test_two_saves_in_a_row(self):
        self.write("file.txt", b"old\n")
        first, second = lines_of_text(150000, "one"), lines_of_text(777, "two")
        result = self.edit(["file.txt"], lambda editor: (editor.save("file.txt", first), editor.save("file.txt", second)))
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual(self.read("file.txt"), second)

    def test_two_files_saved_in_reverse_order(self):
        self.write("a.txt", b"a\n")
        self.write("b.txt", b"b\n")
        new_a, new_b = lines_of_text(5000, "A"), lines_of_text(9000, "B")
        result = self.edit(["a.txt", "b.txt"], lambda editor: (editor.save("b.txt", new_b, ONE_SEGMENT),
                                                               editor.save("a.txt", new_a, ONE_SEGMENT)))
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual((self.read("a.txt"), self.read("b.txt")), (new_a, new_b))

    def test_save_and_close_in_one_segment(self):
        self.write("file.txt", b"old\n")
        new = lines_of_text(400)
        result = self.edit(["file.txt"], lambda editor: editor.send(
            b"save\ntoken: file.txt\ndata: %d\n" % len(new) + new + b"\nclose\ntoken: file.txt\n\n"))
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual(self.read("file.txt"), new)

    def test_crlf_line_endings_in_headers(self):
        self.write("file.txt", b"old\n")
        result = self.edit(["file.txt"], lambda editor: editor.send(
            b"save\r\ntoken: file.txt\r\ndata: 4\r\nnew\n\r\nclose\r\ntoken: file.txt\r\n\r\n"))
        self.assertEqual(result.status, 0, result.stderr)
        self.assertEqual(self.read("file.txt"), b"new\n")

    def test_header_line_split_across_packets(self):
        self.write("file.txt", b"old\n")

        def script(editor):
            editor.send(b"save\ntoken: fi", gap=0.3)
            editor.send(b"le.txt\ndata: 4\nnew\n\n")

        result = self.edit(["file.txt"], script)
        self.assertEqual(self.read("file.txt"), b"new\n")
        self.assertEqual(result.status, 0, result.stderr)

    @unittest.skipIf(os.geteuid() == 0, "root may write to read-only files")
    def test_failed_save_does_not_derail_the_next_one(self):
        os.chmod(self.write("ro.txt", b"read-only\n"), 0o444)
        self.write("rw.txt", b"old\n")
        result = self.edit(["ro.txt", "rw.txt"], lambda editor: (editor.save("ro.txt", lines_of_text(20000)),
                                                                 editor.save("rw.txt", b"new\n")), options=["-f"])
        self.assertEqual(self.read("ro.txt"), b"read-only\n")
        self.assertEqual(self.read("rw.txt"), b"new\n")
        self.assertNotEqual(result.status, 0, "a failed save must show in the exit status")

    def test_overlong_header_line_ends_the_session(self):
        self.write("file.txt", b"old\n")
        result = self.edit(["file.txt"], lambda editor: editor.send(b"save\ntoken: " + b"A" * 9000 + b"\n"))
        self.assertNotEqual(result.status, 0)
        self.assertEqual(self.read("file.txt"), b"old\n")


class SaveSafetyTest(RmateTestCase):
    """Whatever the editor or the network do, files must not be damaged."""

    def test_connection_lost_mid_save_keeps_the_file(self):
        old, new = lines_of_text(50000, "old"), lines_of_text(60000, "new")
        self.write("file.txt", old)
        result = self.edit(["file.txt"], lambda editor: editor.send(
            b"save\ntoken: file.txt\ndata: %d\n" % len(new) + new[:20000]))
        self.assertEqual(self.read("file.txt"), old)
        self.assertNotEqual(result.status, 0)

    def test_invalid_data_length_keeps_the_file_and_ends_the_session(self):
        for length in (b"abc", b"-1", b"", b"12 ", b"99999999999999999999999999"):
            with self.subTest(length=length):
                self.write("file.txt", b"old\n")
                result = self.edit(["file.txt"], lambda editor: editor.send(
                    b"save\ntoken: file.txt\ndata: " + length + b"\n\n"))
                self.assertEqual(self.read("file.txt"), b"old\n")
                self.assertNotEqual(result.status, 0)
                self.assertIn("Invalid data length", result.stderr)

    def test_payload_of_a_save_without_token_is_not_parsed(self):
        self.write("file.txt", b"old\n")
        payload = b"\nsave\ntoken: file.txt\ndata: 4\nbad\n\n"
        result = self.edit(["file.txt"], lambda editor: editor.send(b"save\ndata: %d\n" % len(payload) + payload + b"\n"))
        self.assertEqual(self.read("file.txt"), b"old\n")
        self.assertNotEqual(result.status, 0)

    @unittest.expectedFailure  # any path the editor names is written, and created if missing
    def test_save_for_a_file_that_was_not_opened_is_refused(self):
        self.write("file.txt", b"opened\n")
        self.write("other.txt", b"not opened\n")
        result = self.edit(["file.txt"], lambda editor: (editor.save("other.txt", b"overwritten\n"),
                                                         editor.save("planted.txt", b"planted\n"),
                                                         editor.save("file.txt", b"saved\n")))
        self.assertEqual(self.read("other.txt"), b"not opened\n")
        self.assertIsNone(self.read("planted.txt"))
        self.assertEqual(self.read("file.txt"), b"saved\n", "the session must carry on")
        self.assertNotEqual(result.status, 0)


class BackgroundTest(RmateTestCase):
    """Without -w, rmate returns at once and a detached process handles the saves."""

    def start(self, editor, **popen_args):
        """Start rmate in background mode. Returns (foreground process, pipe that reports EOF when all is over)."""
        alive, held_open = os.pipe()
        process = subprocess.Popen([RMATE, "-p", str(editor.port), "file.txt"], cwd=self.dir, pass_fds=[held_open],
                                   stdin=subprocess.DEVNULL, **popen_args)
        os.close(held_open)
        self.addCleanup(os.close, alive)
        return process, alive

    def wait_for_eof(self, fd, timeout=TIMEOUT):
        return bool(select.select([fd], [], [], timeout)[0])

    def test_returns_at_once_and_saves_later(self):
        self.write("file.txt", b"old\n")
        editor = FakeEditor()
        process, alive = self.start(editor)
        self.assertEqual(process.wait(TIMEOUT), 0)
        self.assertTrue(editor.wait_for_opens(1))
        new = lines_of_text(50000)
        editor.save("file.txt", new)
        editor.close_file("file.txt")
        editor.hang_up()
        self.assertTrue(self.wait_for_eof(alive), "the background process must exit when the editor hangs up")
        self.assertEqual(self.read("file.txt"), new)

    def test_fails_in_the_foreground_when_no_editor_listens(self):
        self.write("file.txt", b"old\n")
        unused = socket.socket()
        unused.bind(("127.0.0.1", 0))
        port = unused.getsockname()[1]
        unused.close()
        done = subprocess.run([RMATE, "-p", str(port), "file.txt"], cwd=self.dir, timeout=TIMEOUT,
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn(b"Could not connect", done.stderr)

    @unittest.expectedFailure  # the background process keeps stdout and stderr open
    def test_releases_stdio_so_callers_without_a_terminal_return(self):
        # `ssh host rmate file` waits for EOF on rmate's stdout and stderr, not just for its exit.
        self.write("file.txt", b"old\n")
        editor = FakeEditor()
        process, alive = self.start(editor, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(process.stdout.close)
        self.assertEqual(process.wait(TIMEOUT), 0)
        self.assertTrue(editor.wait_for_opens(1))
        released = self.wait_for_eof(process.stdout, timeout=2)
        editor.hang_up()
        self.assertTrue(self.wait_for_eof(alive))
        self.assertTrue(released, "stdout/stderr still open while the editor holds the file")


if __name__ == "__main__":
    if not os.access(RMATE, os.X_OK):
        raise SystemExit("rmate binary not found at %s (build it first, or set RMATE)" % RMATE)
    unittest.main()
