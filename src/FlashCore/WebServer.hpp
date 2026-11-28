#pragma once

#include <string_view>

namespace flash::web
{
inline constexpr std::string_view local_server = R"PY(import argparse
import base64
import hashlib
import http.client
import json
import lzma
import socket
import struct
import threading
import zlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_FRAME = 2 ** 20
MAX_MOVIE = 256 * 1024 * 1024
SOURCE_PREFIX = "/__flashstudio_source"
NETWORK_FILE = ROOT / "__flashstudio" / "network.json"
if not NETWORK_FILE.exists():
    NETWORK_FILE = ROOT / "network.json"
CATALOG_FILE = ROOT / "__flashstudio" / "catalog.json"


def pack_frame(opcode, payload):
    size = len(payload)
    if size < 126:
        header = bytes([size])
    elif size <= 65535:
        header = bytes([126]) + struct.pack("!H", size)
    else:
        header = bytes([127]) + struct.pack("!Q", size)
    return bytes([128 | opcode]) + header + payload


def unpack_movie(data):
    if len(data) < 8 or data[:3] not in (b"FWS", b"CWS", b"ZWS"):
        raise ValueError("Invalid SWF header")
    size, = struct.unpack_from("<I", data, 4)
    if not 15 <= size <= MAX_MOVIE:
        raise ValueError("Invalid SWF length")
    if data[:3] == b"FWS":
        if len(data) < size:
            raise ValueError("Truncated SWF")
        return data[:size]
    if data[:3] == b"CWS":
        decoder = zlib.decompressobj()
        body = decoder.decompress(memoryview(data)[8:], size - 7)
        if not decoder.eof:
            raise ValueError("Invalid compressed SWF")
    else:
        if len(data) < 17:
            raise ValueError("Truncated LZMA header")
        packed, = struct.unpack_from("<I", data, 8)
        if packed not in (len(data) - 12, len(data) - 17):
            raise ValueError("Invalid LZMA length")
        stream = data[12:17] + b"\xff" * 8 + data[17:]
        body = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=MAX_MOVIE).decompress(stream, size - 7)
    if len(body) != size - 8:
        raise ValueError("Invalid SWF body")
    return b"FWS" + data[3:8] + body


class SiteHandler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def send_response(self, code, message=None):
        super().send_response(code, message)
        path = urlsplit(self.path).path
        if code in (200, 304) and path.startswith("/__flashstudio/assets/"):
            name = Path(path).stem
            if len(name) == 64 and all(char in "0123456789abcdef" for char in name):
                self.send_header("Cache-Control", "public, max-age=31536000, immutable")

    def handle(self):
        try:
            super().handle()
        except ConnectionError:
            self.close_connection = True

    def do_GET(self):
        path = urlsplit(self.path).path
        if path.startswith(SOURCE_PREFIX + "/"):
            return self.serve_movie_source(path.removeprefix(SOURCE_PREFIX))
        if not path.startswith("/__flashstudio_socket/"):
            if self.is_static(path):
                return super().do_GET()
            if self.redirect_movie():
                return
            return self.forward_http()

        port = self.server.server_port
        if self.headers.get("Origin") not in {f"http://127.0.0.1:{port}", f"http://localhost:{port}"}:
            return self.send_error(403, "Expected a loopback origin")
        if self.headers.get("Upgrade", "").lower() != "websocket" or self.headers.get("Sec-WebSocket-Version") != "13":
            return self.send_error(426, "WebSocket required")
        key = self.headers.get("Sec-WebSocket-Key", "")
        try:
            if len(base64.b64decode(key, validate=True)) != 16:
                raise ValueError()
        except ValueError:
            return self.send_error(400, "Invalid WebSocket key")

        name = unquote(path.removeprefix("/__flashstudio_socket/"))
        try:
            targets = json.loads(NETWORK_FILE.read_text("utf-8")).get("tcp", {})
            target = targets.get(name)
            if not target:
                return self.send_error(404, "Socket route is not configured")
            host, port = target["host"], target["port"]
            if not isinstance(host, str) or not (1 <= port <= 65535):
                raise ValueError()
            remote = socket.create_connection((host, port), timeout=10)
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            return self.send_error(502, "TCP target unavailable")

        with remote:
            self.close_connection = True
            remote.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            remote.settimeout(None)
            accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
            self.send_response(101, "Switching Protocols")
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            self.wfile.flush()
            self.relay(remote)

    def is_static(self, path):
        target = Path(self.translate_path(path))
        return target.is_file() or (target.is_dir() and (target / "index.html").is_file())

    def do_HEAD(self):
        if self.is_static(urlsplit(self.path).path):
            return super().do_HEAD()
        if self.redirect_movie():
            return
        return self.forward_http()

    def redirect_movie(self):
        request = urlsplit(self.path)
        path = unquote(request.path)
        if path != "/" and not path.lower().endswith(".swf"):
            return False
        try:
            catalog = json.loads(CATALOG_FILE.read_text("utf-8"))
            movies = catalog["movies"]
            page = catalog["entry"] if path == "/" else movies.get(path)
            if not page and path != "/":
                page = next((value for key, value in movies.items() if key.lower() == path.lower()), None)
        except (OSError, ValueError, KeyError, AttributeError):
            return False
        if not page or not page.startswith("/") or page.startswith("//") or not self.is_static(page):
            return False
        self.send_response(302)
        self.send_header("Location", page + ("?" + request.query if request.query else ""))
        self.send_header("Content-Length", "0")
        self.end_headers()
        return True

    def do_POST(self):
        self.forward_http()

    def serve_movie_source(self, path):
        original = unquote(path)
        if not original.lower().endswith(".swf") or any(
                part in ("", ".", "..") for part in original[1:].split("/")) or any(
                c in original for c in "\\\r\n\x00"):
            return self.send_error(404)
        try:
            movies = json.loads(CATALOG_FILE.read_text("utf-8"))["movies"]
        except (OSError, ValueError, KeyError):
            return self.send_error(404)
        if original not in movies:
            return self.send_error(404)
        return self.forward_http(movie_source=path)

    def forward_http(self, movie_source=None):
        path = urlsplit(self.path)
        if path.scheme or path.netloc or not path.path.startswith("/") or (
                not movie_source and path.path.startswith("/__flashstudio")):
            return self.send_error(404)
        origin = self.headers.get("Origin")
        local_port = self.server.server_port
        if self.headers.get("Sec-Fetch-Site") == "cross-site" or (
                origin and origin not in {f"http://127.0.0.1:{local_port}", f"http://localhost:{local_port}"}):
            return self.send_error(403, "Expected a loopback origin")
        try:
            port = json.loads(NETWORK_FILE.read_text("utf-8")).get("backendPort")
        except (OSError, ValueError, AttributeError):
            port = None
        if type(port) is not int or not 1 <= port <= 65535 or port == self.server.server_port:
            return self.send_error(404)
        try:
            size = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return self.send_error(400)
        if not 0 <= size <= 16 * MAX_FRAME or self.headers.get("Transfer-Encoding"):
            self.close_connection = True
            return self.send_error(413)
        headers = {name: self.headers[name] for name in (
            "Accept", "Authorization", "Content-Type", "Cookie", "If-Modified-Since", "Origin", "Referer"
        ) if name in self.headers}
        if movie_source:
            headers.pop("If-Modified-Since", None)
        remote = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        try:
            remote.request(self.command, movie_source or self.path, self.rfile.read(size) if size else None, headers)
            response = remote.getresponse()
        except (OSError, ValueError, http.client.HTTPException):
            remote.close()
            return self.send_error(502, "HTTP backend unavailable")
        try:
            if movie_source:
                if response.status != 200:
                    return self.send_error(response.status, "Original SWF unavailable")
                incoming = response.read(MAX_MOVIE + 1)
                if len(incoming) > MAX_MOVIE:
                    return self.send_error(413, "Original SWF too large")
                try:
                    movie = unpack_movie(incoming)
                except (ValueError, zlib.error, lzma.LZMAError) as error:
                    return self.send_error(502, str(error))
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(movie)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(movie)
                return
            self.send_response(response.status)
            ignore = {"connection", "content-length", "date", "keep-alive", "proxy-authenticate", "proxy-authorization",
                      "server", "te", "trailer", "transfer-encoding", "upgrade"}
            for key, value in response.getheaders():
                if key.lower() not in ignore:
                    self.send_header(key, value)
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            if self.command != "HEAD":
                while block := response.read(65536):
                    self.wfile.write(block)
        except (OSError, http.client.HTTPException):
            self.close_connection = True
        finally:
            remote.close()

    def relay(self, remote):
        write_lock = threading.Lock()

        def send(opcode, data):
            with write_lock:
                self.connection.sendall(pack_frame(opcode, data))

        def download():
            try:
                while data := remote.recv(65536):
                    send(2, data)
                send(8, struct.pack("!H", 1000))
            except OSError:
                pass
            finally:
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

        sender = threading.Thread(target=download, daemon=True)
        sender.start()
        fragmented = False
        message_size = 0
        try:
            while True:
                header = self.rfile.read(2)
                if len(header) != 2:
                    break
                first, second = header
                final, opcode = bool(first & 128), first & 15
                if not second & 128 or first & 112 or opcode not in (0, 1, 2, 8, 9, 10):
                    raise ValueError("Invalid frame")
                length = second & 127
                if length >= 126:
                    width = 2 if length == 126 else 8
                    length = int.from_bytes(self.rfile.read(width), "big")
                if length > MAX_FRAME or (opcode >= 8 and (length > 125 or not final)):
                    raise ValueError("Frame too large")
                key, payload = self.rfile.read(4), self.rfile.read(length)
                if len(key) != 4 or len(payload) != length:
                    break
                data = bytes(byte ^ key[index % 4] for index, byte in enumerate(payload))
                if opcode == 8:
                    send(8, data)
                    break
                if opcode == 9:
                    send(10, data)
                if opcode < 8:
                    if (opcode == 0) != fragmented:
                        raise ValueError("Invalid continuation")
                    message_size += length
                    if message_size > MAX_FRAME:
                        raise ValueError("Message too large")
                    fragmented = not final
                    remote.sendall(data)
                    if final:
                        message_size = 0
        except ValueError:
            try:
                send(8, struct.pack("!H", 1002))
            except OSError:
                pass
        except OSError:
            pass
        finally:
            try:
                remote.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sender.join(timeout=1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8000)
    options = parser.parse_args()
    with ThreadingHTTPServer(("127.0.0.1", options.port), SiteHandler) as server:
        print(f"http://127.0.0.1:{server.server_port}/", flush=True)
        server.serve_forever()
)PY";
}
