import socket
import struct
import time

import pytest

import cgdn1_ota_push


def _fetch(port, name, *, read_all=True, rcvbuf=None):
    """GET the file like the monitor does; return the body bytes received before closing."""
    s = socket.socket()
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.connect(("127.0.0.1", port))
    s.sendall(f"GET /{name} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(512)
    body = buf.split(b"\r\n\r\n", 1)[1]
    if read_all:
        s.settimeout(5)
        while True:
            d = s.recv(65536)
            if not d:
                break
            body += d
        s.close()
    else:  # hang up with a reset while most of the file is still queued at the server
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()
    return body


@pytest.fixture
def served(tmp_path, monkeypatch):
    monkeypatch.setattr(cgdn1_ota_push, "DELIVERY_TIMEOUT", 5)
    monkeypatch.setattr(cgdn1_ota_push, "PEER_CLOSE_TIMEOUT", 5)
    cgdn1_ota_push.DOWNLOADED.clear()
    cgdn1_ota_push.UNDELIVERED.clear()
    servers = []

    def start(size):
        f = tmp_path / "fw.bin"
        f.write_bytes(bytes(range(256)) * (size // 256))
        srv = cgdn1_ota_push.serve_file(f, 0)
        servers.append(srv)
        return f, srv.server_address[1]

    yield start
    for srv in servers:
        srv.shutdown()


def _wait_for(capsys, needle, timeout=10):
    out = ""
    end = time.time() + timeout
    while time.time() < end:
        out += capsys.readouterr().out
        if needle(out):
            return out
        time.sleep(0.05)
    return out


def test_full_download_is_reported_as_acknowledged(served, capsys):
    f, port = served(256 * 1024)
    body = _fetch(port, f.name)
    assert body == f.read_bytes()
    out = _wait_for(capsys, lambda o: "acknowledged all" in o)
    assert f"monitor acknowledged all {len(body)} bytes" in out
    assert cgdn1_ota_push.DOWNLOADED


def test_reset_before_delivery_is_not_reported_as_success(served, capsys):
    f, port = served(4 * 1024 * 1024)
    _fetch(port, f.name, read_all=False, rcvbuf=4096)
    out = _wait_for(capsys, lambda o: "undelivered" in o or "aborted" in o)
    assert "acknowledged all" not in out
    assert "undelivered" in out or "aborted" in out
    assert not cgdn1_ota_push.DOWNLOADED
    assert cgdn1_ota_push.UNDELIVERED
