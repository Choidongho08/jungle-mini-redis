from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import unittest

from examples.cache_api import create_server
from examples.resp_client import RedisClient, RedisError, encode


SERVER = Path(os.environ.get("MINI_REDIS_BIN", "build/mini-redis")).resolve()


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.server = subprocess.Popen(
            [SERVER, "127.0.0.1", "0"],
            stdout=subprocess.PIPE, text=True,
        )
        self.addCleanup(self.stop_server)
        line = self.server.stdout.readline()
        self.assertTrue(line.startswith("Listening on 127.0.0.1:"), line)
        self.port = int(line.rsplit(":", 1)[-1])

    def stop_server(self):
        self.server.terminate()
        try:
            self.server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.server.kill()
            self.server.wait()
        finally:
            self.server.stdout.close()

    def client(self):
        return RedisClient("127.0.0.1", self.port)

    def test_commands_and_binary_strings(self):
        with self.client() as redis:
            self.assertEqual(redis.command("ping"), b"PONG")
            self.assertEqual(redis.command("PING", "hello"), b"hello")
            self.assertIsNone(redis.command("GET", b""))
            self.assertEqual(redis.command("SET", b"", b""), b"OK")
            self.assertEqual(redis.command("GET", b""), b"")
            key, value = b"k\x00", b"a\x00\r\n\xff"
            self.assertEqual(redis.command("SET", key, value), b"OK")
            self.assertEqual(redis.command("GET", key), value)
            self.assertEqual(redis.command("DEL", key, key, b"", "missing"), 2)
            self.assertIsNone(redis.command("GET", key))

    def test_expiry_and_bad_commands(self):
        with self.client() as redis:
            for invalid in (0, -1, "abc", "9223372036854775808"):
                with self.assertRaisesRegex(RedisError, "expire time"):
                    redis.command("SET", "key", "value", "PX", invalid)
            with self.assertRaisesRegex(RedisError, "wrong number"):
                redis.command("GET")
            with self.assertRaisesRegex(RedisError, "unknown command"):
                redis.command("MISSING")
            redis.command("SET", "key", "old", "PX", 500)
            self.assertIn(redis.command("PTTL", "key"), range(1, 501))
            deadline = time.monotonic() + 2
            while redis.command("PTTL", "key") != -2:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            self.assertIsNone(redis.command("GET", "key"))
            self.assertEqual(redis.command("DEL", "key"), 0)
            redis.command("SET", "key", "old", "EX", 60)
            redis.command("SET", "key", "new")
            self.assertEqual(redis.command("PTTL", "key"), -1)
            self.assertEqual(redis.command("GET", "key"), b"new")

    def test_fragmented_and_pipelined_requests(self):
        with self.client() as redis:
            data = encode("SET", "key", "value")
            redis.socket.sendall(data[:-1])
            with self.client() as other:
                self.assertIsNone(other.command("GET", "key"))
            redis.socket.sendall(data[-1:] + encode("GET", "key") + encode("DEL", "key"))
            self.assertEqual(redis.read_reply(), b"OK")
            self.assertEqual(redis.read_reply(), b"value")
            self.assertEqual(redis.read_reply(), 1)

    def test_large_value_and_invalid_frames(self):
        with self.client() as redis:
            value = b"x" * 250000
            self.assertEqual(redis.command("SET", "large", value), b"OK")
            self.assertEqual(redis.command("GET", "large"), value)

        for invalid in (b"*1\r\n$999999\r\n", b"*1\r\n$4\r\nPINGxx", b"*0\r\n"):
            with self.client() as redis:
                redis.socket.sendall(invalid)
                with self.assertRaisesRegex(RedisError, "invalid RESP"):
                    redis.read_reply()
                self.assertEqual(redis.reader.read(), b"")

        with self.client() as redis:
            self.assertEqual(redis.command("PING"), b"PONG")

    def test_long_pipeline_preserves_command_and_error_order(self):
        with self.client() as redis:
            frames, expected = [], []
            for i in range(100):
                value = f"value:{i}".encode()
                frames.extend((encode("SET", "key", value), encode("GET", "key"),
                               encode("DEL", "key"), encode("GET", "key")))
                expected.extend((b"OK", value, 1, None))
            redis.socket.sendall(b"".join(frames))
            for value in expected:
                self.assertEqual(redis.read_reply(), value)

            # 명령 오류는 연결을 유지하고, 다음 명령의 응답도 순서대로 보낸다.
            redis.socket.sendall(encode("PING") + encode("GET") + encode("PING", "after"))
            self.assertEqual(redis.read_reply(), b"PONG")
            with self.assertRaisesRegex(RedisError, "wrong number"):
                redis.read_reply()
            self.assertEqual(redis.read_reply(), b"after")

            # 완성된 응답 뒤에 미완성 요청이 있어도 앞선 응답부터 전송한다.
            partial = encode("SET", "fragment", "complete")
            redis.socket.sendall(encode("PING") + partial[:-1])
            self.assertEqual(redis.read_reply(), b"PONG")
            with self.client() as other:
                self.assertIsNone(other.command("GET", "fragment"))
            redis.socket.sendall(partial[-1:] + encode("GET", "fragment"))
            self.assertEqual(redis.read_reply(), b"OK")
            self.assertEqual(redis.read_reply(), b"complete")

            # 정상 응답 뒤에 프로토콜 오류를 보내고, 그 이후 명령은 실행하지 않는다.
            redis.socket.sendall(encode("PING") * 3 + b"*0\r\n" +
                                 encode("SET", "after-error", "bad"))
            for _ in range(3):
                self.assertEqual(redis.read_reply(), b"PONG")
            with self.assertRaisesRegex(RedisError, "invalid RESP"):
                redis.read_reply()
            self.assertEqual(redis.reader.read(), b"")
        with self.client() as redis:
            self.assertIsNone(redis.command("GET", "after-error"))

    def test_large_pipeline_and_slow_reader_do_not_block_other_clients(self):
        with self.client() as slow:
            value = bytes(range(256)) * 976
            self.assertEqual(slow.command("SET", "large-pipeline", value), b"OK")
            # 응답을 아직 읽지 않는 연결이 있어도 다른 연결은 처리할 수 있다.
            slow.socket.sendall(encode("GET", "large-pipeline") * 40 +
                                encode("DEL", "large-pipeline") +
                                encode("GET", "large-pipeline"))
            with self.client() as other:
                self.assertEqual(other.command("PING"), b"PONG")
                self.assertEqual(other.command("SET", "independent", "ok"), b"OK")
            for _ in range(40):
                self.assertEqual(slow.read_reply(), value)
            self.assertEqual(slow.read_reply(), 1)
            self.assertIsNone(slow.read_reply())

    def test_concurrent_writes_and_shared_delete(self):
        workers = 8
        barrier = threading.Barrier(workers, timeout=5)
        with self.client() as redis:
            redis.command("SET", "shared", "value")

        def run(worker):
            with self.client() as redis:
                for i in range(30):
                    key = f"{worker}:{i}"
                    self.assertEqual(redis.command("SET", key, key), b"OK")
                    self.assertEqual(redis.command("GET", key), key.encode())
                barrier.wait()
                return redis.command("DEL", "shared")

        with ThreadPoolExecutor(max_workers=workers) as pool:
            self.assertEqual(sum(pool.map(run, range(workers))), 1)

    def test_http_cache_hit_and_invalidation(self):
        api = create_server(("127.0.0.1", 0), ("127.0.0.1", self.port), 0, 60000, quiet=True)
        thread = threading.Thread(target=api.serve_forever)
        thread.start()
        self.addCleanup(api.server_close)
        self.addCleanup(thread.join, 3)
        self.addCleanup(api.shutdown)
        connection = http.client.HTTPConnection("127.0.0.1", api.server_port, timeout=3)
        self.addCleanup(connection.close)

        def request(method, path):
            connection.request(method, path)
            response = connection.getresponse()
            body = response.read()
            self.assertEqual(response.status, 200, body)
            return response.getheader("X-Cache"), body

        _, origin = request("GET", "/uncached/7")
        self.assertEqual(request("GET", "/cached/7"), ("MISS", origin))
        with self.client() as redis:
            self.assertEqual(redis.command("GET", "item:7"), origin)
        self.assertEqual(request("GET", "/cached/7"), ("HIT", origin))
        _, body = request("DELETE", "/cached/7")
        self.assertEqual(json.loads(body), {"deleted": 1})
        self.assertEqual(request("GET", "/cached/7"), ("MISS", origin))
