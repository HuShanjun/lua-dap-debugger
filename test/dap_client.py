import json
import socket


class DapClient:
    def __init__(self, host="127.0.0.1", port=8172, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.seq = 0

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send_request(self, command, arguments=None):
        self.seq += 1
        body = {
            "seq": self.seq,
            "type": "request",
            "command": command,
            "arguments": arguments or {},
        }
        data = json.dumps(body).encode("utf-8")
        header = f"Content-Length: {len(data)}\r\n\r\n".encode("ascii")
        self.sock.sendall(header + data)
        return self.seq

    def read_message(self):
        while True:
            idx = self.buf.find(b"\r\n\r\n")
            if idx >= 0:
                header = self.buf[:idx].decode("ascii", errors="replace")
                length = None
                for line in header.split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        length = int(line.split(":", 1)[1].strip())
                        break
                if length is None:
                    raise RuntimeError(f"missing Content-Length: {header!r}")
                start = idx + 4
                if len(self.buf) >= start + length:
                    body = self.buf[start : start + length]
                    self.buf = self.buf[start + length :]
                    return json.loads(body.decode("utf-8"))
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("socket closed")
            self.buf += chunk

    def wait_for(self, pred, limit=20):
        for _ in range(limit):
            msg = self.read_message()
            if pred(msg):
                return msg
        raise TimeoutError("wait_for exceeded limit")
