"""HTTP example with a simulated slow data source."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
import time

from .resp_client import RedisClient, RedisError


class CacheHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    disable_nagle_algorithm = True

    def setup(self):
        super().setup()
        self.redis = None

    def finish(self):
        try:
            if self.redis is not None:
                self.redis.close()
        finally:
            super().finish()

    def log_message(self, format, *args):
        if not self.server.quiet:
            super().log_message(format, *args)

    def redis_command(self, *parts):
        try:
            if self.redis is None:
                self.redis = RedisClient(*self.server.redis_address)
            return self.redis.command(*parts)
        except OSError:
            if self.redis is not None:
                self.redis.close()
                self.redis = None
            raise

    def reply(self, status, body, cache=None):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if cache is not None:
            self.send_header("X-Cache", cache)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        route = re.fullmatch(r"/(cached|uncached)/([a-zA-Z0-9_-]{1,64})", self.path)
        if not route:
            self.reply(404, b'{"error":"not found"}')
            return

        mode, item = route.groups()
        key = f"item:{item}"
        try:
            if mode == "cached":
                body = self.redis_command("GET", key)
                if body is not None:
                    self.reply(200, body, "HIT")
                    return

            time.sleep(self.server.origin_delay)
            body = json.dumps({"id": item, "name": f"Item {item}"}).encode()
            if mode == "cached":
                self.redis_command("SET", key, body, "PX", self.server.ttl_ms)
        except (OSError, RedisError):
            self.reply(503, b'{"error":"cache unavailable"}')
            return

        self.reply(200, body, "MISS" if mode == "cached" else "BYPASS")

    def do_DELETE(self):
        route = re.fullmatch(r"/cached/([a-zA-Z0-9_-]{1,64})", self.path)
        if not route:
            self.reply(404, b'{"error":"not found"}')
            return
        try:
            deleted = self.redis_command("DEL", f"item:{route[1]}")
        except (OSError, RedisError):
            self.reply(503, b'{"error":"cache unavailable"}')
            return
        self.reply(200, json.dumps({"deleted": deleted}).encode())


def create_server(http_address, redis_address, origin_delay, ttl_ms, quiet=False):
    server = ThreadingHTTPServer(http_address, CacheHandler)
    server.redis_address = redis_address
    server.origin_delay = origin_delay
    server.ttl_ms = ttl_ms
    server.quiet = quiet
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--redis-host", default="127.0.0.1")
    parser.add_argument("--redis-port", type=int, default=6379)
    parser.add_argument("--origin-delay-ms", type=float, default=20)
    parser.add_argument("--ttl-ms", type=int, default=60000)
    args = parser.parse_args()
    with create_server(
        ("127.0.0.1", args.port),
        (args.redis_host, args.redis_port),
        args.origin_delay_ms / 1000,
        args.ttl_ms,
    ) as server:
        print(f"HTTP example on http://127.0.0.1:{server.server_port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
