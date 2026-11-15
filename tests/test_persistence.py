from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import time
import unittest

from examples.resp_client import RedisClient


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


class PersistenceTests(unittest.TestCase):
    def test_snapshot_then_replay_writes_after_crash(self):
        with TemporaryDirectory() as directory:
            with running_server(directory, crash=True) as client:
                for number in range(13):
                    self.assertEqual(client.command("SET", f"key:{number}", number), b"OK")
                self.assertEqual(client.command("SAVE"), b"OK")
                self.assertTrue((Path(directory) / "snapshot.rdb").is_file())
                self.assertEqual(client.command("SET", "key:0", b"new\x00\r\n"), b"OK")
                self.assertEqual(client.command("DEL", "key:1", "key:2"), 2)
                self.assertEqual(client.command("SET", b"", b""), b"OK")
            with running_server(directory) as client:
                self.assertEqual(client.command("GET", "key:0"), b"new\x00\r\n")
                self.assertIsNone(client.command("GET", "key:1"))
                self.assertIsNone(client.command("GET", "key:2"))
                for number in range(3, 13):
                    self.assertEqual(client.command("GET", f"key:{number}"), str(number).encode())
                self.assertEqual(client.command("GET", b""), b"")
            with running_server(directory) as client:
                self.assertEqual(client.command("GET", "key:0"), b"new\x00\r\n")
                self.assertIsNone(client.command("GET", "key:1"))

    def test_log_only_recovers_and_removes_incomplete_tail(self):
        with TemporaryDirectory() as directory:
            with running_server(directory, crash=True) as client:
                self.assertEqual(client.command("SET", "before", "value"), b"OK")
            self.assertFalse((Path(directory) / "snapshot.rdb").exists())
            log = Path(directory) / "appendonly.aof"
            length = log.stat().st_size
            with log.open("ab") as file:
                file.write(b"\x00\x00\x00\x0dS")
            with running_server(directory, crash=True) as client:
                self.assertEqual(log.stat().st_size, length)
                self.assertEqual(client.command("GET", "before"), b"value")
                self.assertEqual(client.command("SET", "after", "value"), b"OK")
            with running_server(directory) as client:
                self.assertEqual(client.command("GET", "after"), b"value")

    def test_downtime_counts_toward_ttl_and_expired_overwrite_hides_old_value(self):
        with TemporaryDirectory() as directory:
            with running_server(directory, crash=True) as client:
                self.assertEqual(client.command("SET", "key", "old"), b"OK")
                self.assertEqual(client.command("SET", "long", "value", "PX", 30000), b"OK")
                self.assertEqual(client.command("SAVE"), b"OK")
                self.assertEqual(client.command("SET", "key", "new", "PX", 50), b"OK")
            time.sleep(0.1)
            with running_server(directory) as client:
                self.assertIsNone(client.command("GET", "key"))
                self.assertEqual(client.command("PTTL", "key"), -2)
                self.assertIn(client.command("PTTL", "long"), range(1, 29951))
