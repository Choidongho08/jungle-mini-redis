"""Small RESP2 client for the HTTP cache example and its test."""

import socket


class RedisError(Exception):
    pass


def encode(*parts):
    encoded = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        data = part if isinstance(part, bytes) else str(part).encode()
        encoded.extend((f"${len(data)}\r\n".encode(), data, b"\r\n"))
    return b"".join(encoded)


class RedisClient:
    def __init__(self, host, port):
        self.socket = socket.create_connection((host, port), timeout=3)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = self.socket.makefile("rb")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        self.reader.close()
        self.socket.close()

    def command(self, *parts):
        self.socket.sendall(encode(*parts))
        return self.read_reply()

    def read_reply(self):
        line = self.reader.readline()
        if not line.endswith(b"\r\n"):
            raise ConnectionError("Incomplete Redis reply")
        prefix, value = line[:1], line[1:-2]
        if prefix == b"+":
            return value
        if prefix == b"-":
            raise RedisError(value.decode())
        if prefix == b":":
            return int(value)
        if prefix == b"$":
            size = int(value)
            if size == -1:
                return None
            data = self.reader.read(size)
            if len(data) == size and self.reader.read(2) == b"\r\n":
                return data
        raise ConnectionError("Invalid Redis reply")
