from contextlib import contextmanager
import os
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory
import time
import unittest

from examples.resp_client import RedisClient, RedisError


SERVER = Path(os.environ.get("MINI_REDIS_BIN", "build/mini-redis")).resolve()


@contextmanager
def running_server(directory, crash=False):
    with subprocess.Popen([SERVER, "127.0.0.1", "0", directory],
                          stdout=subprocess.PIPE, text=True) as process:
        try:
            line = process.stdout.readline()
            if not line.startswith("Listening on 127.0.0.1:"):
                raise RuntimeError(f"Mini Redis failed to start: {line!r}")
            with RedisClient("127.0.0.1", int(line.rsplit(":", 1)[1])) as client:
                yield client
        finally:
            if crash:
                process.kill()
            else:
                process.terminate()
            result = process.wait(timeout=5)
            if not crash and result != 0:
                raise RuntimeError(f"Mini Redis shutdown failed: {result}")


def old_record(key, value):
    contents = b"S" + struct.pack(">QI", 0, len(key)) + key + value
    return struct.pack(">I", len(contents)) + contents


class PersistenceTests(unittest.TestCase):
    def test_restart_recovers_only_the_last_snapshot_after_crash(self):
        with TemporaryDirectory() as directory:
            data = Path(directory)
            snapshot = data / "dump.rdb"
            with running_server(directory, crash=True) as client:
                self.assertEqual(client.command("SET", "cached", b"old\x00\r\n"), b"OK")
                self.assertEqual(client.command("SET", "deleted", "value"), b"OK")
                self.assertEqual(client.command("SET", b"", b""), b"OK")
                self.assertEqual(client.command("SAVE"), b"OK")
                saved = snapshot.read_bytes()
                self.assertEqual(client.command("SET", "cached", "new"), b"OK")
                self.assertEqual(client.command("SET", "unsaved", "value"), b"OK")
                self.assertEqual(client.command("DEL", "deleted"), 1)
                self.assertEqual(snapshot.read_bytes(), saved)
                self.assertEqual(list(data.glob("*.aof")), [])
                self.assertFalse((data / "persistence.manifest").exists())
            with running_server(directory, crash=True) as client:
                self.assertEqual(client.command("GET", "cached"), b"old\x00\r\n")
                self.assertEqual(client.command("GET", "deleted"), b"value")
                self.assertIsNone(client.command("GET", "unsaved"))
                self.assertEqual(client.command("GET", b""), b"")

    def test_failed_save_keeps_previous_snapshot_and_can_be_retried(self):
        with TemporaryDirectory() as directory:
            data = Path(directory)
            temp = data / "dump.rdb.tmp"
            with running_server(directory, crash=True) as client:
                self.assertEqual(client.command("SET", "key", "before"), b"OK")
                self.assertEqual(client.command("SAVE"), b"OK")
                temp.mkdir()
                self.assertEqual(client.command("SET", "key", "after"), b"OK")
                with self.assertRaisesRegex(RedisError, "snapshot failed"):
                    client.command("SAVE")
                self.assertEqual(client.command("GET", "key"), b"after")
            temp.rmdir()
            with running_server(directory, crash=True) as client:
                self.assertEqual(client.command("GET", "key"), b"before")
                self.assertEqual(client.command("SET", "key", "after"), b"OK")
                self.assertEqual(client.command("SAVE"), b"OK")
            with running_server(directory, crash=True) as client:
                self.assertEqual(client.command("GET", "key"), b"after")

    def test_legacy_snapshots_are_loaded_and_normal_shutdown_writes_new_format(self):
        for filename in ("snapshot.rdb", "snapshot.1.rdb"):
            with self.subTest(filename=filename), TemporaryDirectory() as directory:
                data = Path(directory)
                snapshot = data / filename
                if filename == "snapshot.1.rdb":
                    (data / "persistence.manifest").write_text("1\n")
                    log = data / "appendonly.1.aof"
                else:
                    log = data / "appendonly.aof"
                prefix = b"MRA1" + old_record(b"cached", b"old")
                tail = old_record(b"cached", b"from log") + old_record(b"log-only", b"value")
                log.write_bytes(prefix + tail)
                snapshot.write_bytes(b"MRS1" + struct.pack(">Q", len(prefix)) + prefix[4:])

                with running_server(directory) as client:
                    self.assertEqual(client.command("GET", "cached"), b"old")
                    self.assertIsNone(client.command("GET", "log-only"))
                    self.assertEqual(client.command("SET", "cached", "new"), b"OK")
                self.assertTrue((data / "dump.rdb").read_bytes().startswith(b"MRS2"))
                self.assertEqual(log.read_bytes(), prefix + tail)
                with running_server(directory, crash=True) as client:
                    self.assertEqual(client.command("GET", "cached"), b"new")

    def test_log_only_directory_starts_empty(self):
        with TemporaryDirectory() as directory:
            log = Path(directory) / "appendonly.aof"
            contents = b"MRA1" + old_record(b"key", b"value")
            log.write_bytes(contents)
            with running_server(directory, crash=True) as client:
                self.assertIsNone(client.command("GET", "key"))
            self.assertEqual(log.read_bytes(), contents)

    def test_incomplete_snapshot_fails_startup(self):
        with TemporaryDirectory() as directory:
            (Path(directory) / "dump.rdb").write_bytes(b"MRS2" + old_record(b"key", b"value")[:-1])
            result = subprocess.run([SERVER, "127.0.0.1", "0", directory],
                                    capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"persistence recovery", result.stderr)

    def test_restart_excludes_expired_keys_and_preserves_remaining_ttl(self):
        with TemporaryDirectory() as directory:
            with running_server(directory, crash=True) as client:
                self.assertEqual(client.command("SET", "short", "value", "PX", 150), b"OK")
                self.assertEqual(client.command("SET", "long", "value", "PX", 30000), b"OK")
                self.assertEqual(client.command("SAVE"), b"OK")
            time.sleep(0.2)
            with running_server(directory, crash=True) as client:
                self.assertIsNone(client.command("GET", "short"))
                self.assertEqual(client.command("PTTL", "short"), -2)
                self.assertEqual(client.command("GET", "long"), b"value")
                self.assertIn(client.command("PTTL", "long"), range(1, 29801))
