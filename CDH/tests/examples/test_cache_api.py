"""HTTP 캐시 예제(examples/cache_api.py)를 실제 mini-redis-server 에 붙여 확인한다.

원본: feat/mini-redis-poll-persistence 브랜치(#4)의 tests/test_integration.py 중
test_http_cache_hit_and_invalidation. CDH 서버를 띄우도록 옮겼다.

    make -C CDH example-test
    PYTHONPATH=CDH python3 CDH/tests/examples/test_cache_api.py -v
"""

import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
import unittest

from examples.cache_api import create_server
from examples.resp_client import RedisClient

ROOT = Path(__file__).resolve().parents[3]
SERVER = Path(os.environ.get("SERVER_BIN", ROOT / "build/cdh/default/mini-redis-server")).resolve()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class CacheApiTests(unittest.TestCase):
    def setUp(self):
        self.port = free_port()
        self.server = subprocess.Popen(
            [SERVER, "--port", str(self.port), "--log-level", "warn"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        self.addCleanup(self.stop_server)
        deadline = time.monotonic() + 5
        while True:
            try:
                with RedisClient("127.0.0.1", self.port) as redis:
                    if redis.command("PING") == b"PONG":
                        return
            except OSError:
                if self.server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"{SERVER} did not start")
                time.sleep(0.05)

    def stop_server(self):
        self.server.terminate()
        self.assertEqual(self.server.wait(timeout=3), 0)

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
        with RedisClient("127.0.0.1", self.port) as redis:
            self.assertEqual(redis.command("GET", "item:7"), origin)
            self.assertGreater(redis.command("PTTL", "item:7"), 0)
        self.assertEqual(request("GET", "/cached/7"), ("HIT", origin))
        _, body = request("DELETE", "/cached/7")
        self.assertEqual(json.loads(body), {"deleted": 1})
        self.assertEqual(request("GET", "/cached/7"), ("MISS", origin))


if __name__ == "__main__":
    unittest.main()
