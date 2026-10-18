"""Push a firmware image to a stock Qingping Air Monitor Lite (CGDN1) over its local MQTT channel.

The stock firmware (4.8.x) accepts a type "23" message on its MQTT "down" topic: it downloads the URL
itself, writes it to the inactive OTA slot, checks only a 4-byte CRC-32 trailer, switches the boot slot
and reboots. There is NO signature check and NO rollback: an image that doesn't boot can only be
recovered over UART (the case has no UART pads broken out). See docs/install.md before using this.

Usage:
  cgdn1_ota_push.py FIRMWARE --broker HOST --mac MAC [options]

FIRMWARE is a local .bin (served from this PC over HTTP while the monitor downloads it) or an
http:// URL the monitor can reach. Serving locally needs this PC's firewall to accept TCP
--http-port from the monitor. Local files are checked before anything is sent: ESP32 image
header, segment checksum and SHA-256, and the CRC trailer.

Options:
  --append-crc      add the CRC trailer (writes FIRMWARE.crc.bin and pushes that); needed for
                    ESPHome's firmware.ota.bin
  --ota-type N      1 = ESP32 app (default), 0 = co-MCU, 5 = HomeKit info
  --dry-run         validate and print the message, send nothing
  --yes             skip the typed confirmation
  --broker HOST     MQTT broker the monitor is connected to (required)
  --port N          MQTT broker port (default 1883)
  --user NAME       broker user (default: qingping)
  --pw-file FILE    file holding the broker password (default: $CGDN1_MQTT_PASSWORD, else none)
  --mac MAC         the monitor's Wi-Fi MAC, e.g. CCB5D1xxxxxx (required; used in the topics)
  --topic-down T    topic the monitor reads (default: qingping/{mac}/down)
  --topic-up T      topic the monitor writes (default: qingping/{mac}/up)
  --http-port N     port for serving a local file (default 8070)
  --timeout S       seconds to watch for the result (default 600)
"""
import argparse, fcntl, hashlib, http.server, json, os, socket, struct, sys, termios, threading, time, zlib
from pathlib import Path

import paho.mqtt.client as mqtt

APP_DESC_MAGIC = 0xABCD5432
DELIVERY_TIMEOUT = 300   # s to wait for the monitor to acknowledge the last byte after write() returns
PEER_CLOSE_TIMEOUT = 60  # s to then wait for the monitor to close its end
PROGRESS_EVERY = 5       # s between delivery progress lines


def crc_trailer_ok(data):
    """The monitor's arch_ota_check_crc: last 4 bytes (LE) == reflected CRC-32, init 0, no final xor."""
    return len(data) > 4 and struct.unpack("<I", data[-4:])[0] == crc32_qp(data[:-4])


def crc32_qp(data):
    return ~zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF


def check_esp_image(img):
    """Validate an esp-idf app image (without the CRC trailer). Returns a dict of facts or raises ValueError."""
    if len(img) < 24 or img[0] != 0xE9:
        raise ValueError("not an ESP32 app image (first byte must be 0xE9)")
    segments, entry = img[1], struct.unpack_from("<I", img, 4)[0]
    chip_id, hash_appended = struct.unpack_from("<H", img, 12)[0], img[23]
    if chip_id != 0:
        raise ValueError(f"image is for chip id {chip_id}, not the classic ESP32 (0)")
    pos, checksum, first_seg = 24, 0xEF, None
    for _ in range(segments):
        if pos + 8 > len(img):
            raise ValueError("image truncated in a segment header")
        load, size = struct.unpack_from("<II", img, pos)
        seg = img[pos + 8:pos + 8 + size]
        if len(seg) != size:
            raise ValueError("image truncated in segment data")
        first_seg = first_seg if first_seg is not None else seg
        for b in seg:
            checksum ^= b
        pos += 8 + size
    pos += 15 - (pos % 16)  # pad so the checksum byte ends a 16-byte block
    if pos >= len(img) or img[pos] != checksum:
        raise ValueError("segment checksum mismatch (corrupt image)")
    end = pos + 1
    if hash_appended:
        if hashlib.sha256(img[:end]).digest() != img[end:end + 32]:
            raise ValueError("appended SHA-256 mismatch (corrupt image)")
        end += 32
    facts = {"segments": segments, "entry": hex(entry), "image_end": end, "extra_bytes": len(img) - end}
    if first_seg and len(first_seg) >= 256 and struct.unpack_from("<I", first_seg, 0)[0] == APP_DESC_MAGIC:
        field = lambda off, n: first_seg[off:off + n].split(b"\0")[0].decode(errors="replace")
        facts.update(version=field(16, 32), project=field(48, 32), built=f"{field(96, 16)} {field(80, 16)}", idf=field(112, 32))
    return facts


def lan_ip_towards(host, port=1883):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, port))
        return s.getsockname()[0]
    finally:
        s.close()


DOWNLOADED = []  # time.time() of each download the monitor acknowledged in full
UNDELIVERED = []  # time.time() of each download that ended with bytes the monitor never acknowledged


def _unacked(sock):
    """Bytes the kernel still holds for this connection: unsent plus sent-but-unacknowledged (Linux SIOCOUTQ)."""
    return struct.unpack("i", fcntl.ioctl(sock.fileno(), termios.TIOCOUTQ, b"\0" * 4))[0]


def _await_delivery(sock, total):
    """Block until the monitor has acknowledged every byte; return the count still undelivered (0 = all).

    write() returns as soon as the kernel has queued the body, long before the monitor (which reads 510
    bytes at a time between flash writes) has it. Closing then would orphan the socket with the tail still
    queued, and an orphaned socket that gets reset loses that tail: the monitor's download loop then gives
    up silently at 99% (no CRC check, no reboot). So keep the connection owned until everything is acked.
    """
    t0 = last = time.time()
    while True:
        left = _unacked(sock)
        if left == 0:
            return 0
        err = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        if err:
            print(f"{time.strftime('%T')} HTTP  connection failed ({os.strerror(err)})", flush=True)
            return left
        now = time.time()
        if now - t0 > DELIVERY_TIMEOUT:
            print(f"{time.strftime('%T')} HTTP  gave up waiting after {DELIVERY_TIMEOUT} s", flush=True)
            return left
        if now - last >= PROGRESS_EVERY:
            last = now
            print(f"{time.strftime('%T')} HTTP  monitor has {total - left}/{total} bytes "
                  f"({100 * (total - left) // total}%)", flush=True)
        time.sleep(0.1)


def _await_peer_close(sock):
    """After our FIN, wait for the monitor to close its end so the socket is never left orphaned."""
    sock.settimeout(PEER_CLOSE_TIMEOUT)
    try:
        while sock.recv(4096):
            pass
    except OSError:
        pass


def serve_file(path, port):
    """Serve exactly one file (with Content-Length, which the monitor needs) and log each request.

    Reports delivery by what the monitor ACKed, not by what write() queued, and keeps each connection
    open until the monitor closes it.
    """
    body, name = path.read_bytes(), path.name

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.lstrip("/") != name:
                self.send_error(404)
                return
            print(f"{time.strftime('%T')} HTTP  {self.client_address[0]} downloading {name} ({len(body)} bytes)", flush=True)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError) as e:
                print(f"{time.strftime('%T')} HTTP  download aborted by the monitor ({e})", flush=True)
                UNDELIVERED.append(time.time())
                return
            print(f"{time.strftime('%T')} HTTP  queued all {len(body)} bytes; waiting for the monitor to "
                  "acknowledge them", flush=True)
            left = _await_delivery(self.connection, len(body))
            if left:
                print(f"{time.strftime('%T')} HTTP  {left} of {len(body)} bytes undelivered: the monitor "
                      "will stop at 99% without checking the CRC or rebooting; push again", flush=True)
                UNDELIVERED.append(time.time())
                return
            DOWNLOADED.append(time.time())
            print(f"{time.strftime('%T')} HTTP  monitor acknowledged all {len(body)} bytes; it now checks "
                  "the CRC and reboots", flush=True)
            try:
                self.connection.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            _await_peer_close(self.connection)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("firmware")
    ap.add_argument("--append-crc", action="store_true")
    ap.add_argument("--ota-type", type=int, default=1, choices=(0, 1, 5))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--yes", action="store_true")
    ap.add_argument("--broker", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user", default="qingping")
    ap.add_argument("--pw-file")
    ap.add_argument("--mac", required=True, type=lambda v: v.replace(":", "").replace("-", "").upper())
    ap.add_argument("--topic-down", default="qingping/{mac}/down")
    ap.add_argument("--topic-up", default="qingping/{mac}/up")
    ap.add_argument("--http-port", type=int, default=8070)
    ap.add_argument("--timeout", type=int, default=600)
    a = ap.parse_args()

    local = not a.firmware.startswith(("http://", "https://"))
    if local:
        path = Path(a.firmware).resolve()
        data = path.read_bytes()
        if a.append_crc:
            if crc_trailer_ok(data):
                sys.exit("file already ends in a valid CRC trailer; drop --append-crc")
            data += struct.pack("<I", crc32_qp(data))
            path = path.with_suffix(".crc.bin")
            path.write_bytes(data)
            print(f"wrote {path} with the CRC trailer appended")
        if not crc_trailer_ok(data):
            sys.exit("CRC trailer missing or wrong; the monitor would reject it after downloading "
                     "(use --append-crc for an image that has none)")
        if a.ota_type == 1:
            try:
                facts = check_esp_image(data[:-4])
            except ValueError as e:
                sys.exit(f"refusing to push: {e}")
            if facts["extra_bytes"]:
                sys.exit(f"refusing to push: {facts['extra_bytes']} unexpected bytes after the image")
        else:
            facts = {}
        url = f"http://{lan_ip_towards(a.broker, a.port)}:{a.http_port}/{path.name}"
        print(f"note      this PC must accept TCP {a.http_port} from the monitor (check your firewall)")
        print(f"file      {path} ({len(data)} bytes, CRC trailer ok)")
        for k in ("project", "version", "built", "idf", "segments", "entry"):
            if k in facts:
                print(f"  {k:9} {facts[k]}")
    else:
        url, facts = a.firmware, {}
        print("remote URL: not validated locally; the monitor checks only the CRC trailer after downloading")

    if len(url) > 127:
        sys.exit(f"URL is {len(url)} characters; the monitor copies it into a 128-byte field without a bound")
    msg = {"type": "23", "url": url, "ota_type": a.ota_type}
    down, up = a.topic_down.format(mac=a.mac), a.topic_up.format(mac=a.mac)
    print(f"message   {down} <- {json.dumps(msg)}")
    if a.dry_run:
        print("dry run: nothing sent")
        return

    target = {1: "the ESP32 firmware", 0: "the co-MCU firmware", 5: "the HomeKit info"}[a.ota_type]
    print(f"\nThis replaces {target} on the monitor. There is no signature check and no rollback: an image that\n"
          "doesn't boot can only be recovered over UART (open the case, 3.3 V USB-TTL on the ESP32 pads).\n"
          "Read docs/install.md first.")
    if not a.yes and input('Type "flash" to send it: ').strip() != "flash":
        sys.exit("not sent")

    server = serve_file(path, a.http_port) if local else None
    seen = {"before": None, "after": None}
    try:  # paho-mqtt 2.x requires the callback API version; 1.x has no such argument
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="cgdn1-ota-push")
    except AttributeError:
        c = mqtt.Client(client_id="cgdn1-ota-push")
    password = open(a.pw_file).read().strip() if a.pw_file else os.environ.get("CGDN1_MQTT_PASSWORD")
    c.username_pw_set(a.user, password)

    def on_message(cl, userdata, m):
        try:
            d = json.loads(m.payload)
        except ValueError:
            return
        t = str(d.get("type"))
        if t == "24":
            print(f"{time.strftime('%T')} MQTT  OTA status: {json.dumps(d)}", flush=True)
        elif t == "13":
            v = d.get("sw_version")
            # The monitor says hello (type 13) every minute; one that comes 20 s after the file finished
            # downloading is from the newly booted firmware. With a remote URL we can only report changes.
            if local and DOWNLOADED and time.time() - DOWNLOADED[-1] > 20:
                seen["after"] = v
            elif seen["before"] is None:
                seen["before"] = v
                print(f"{time.strftime('%T')} MQTT  monitor online, running {v}", flush=True)
            elif v != seen["before"]:
                seen["after"] = v

    c.on_message = on_message
    c.connect(a.broker, a.port, 30)
    c.subscribe(up)
    c.loop_start()
    time.sleep(1)
    c.publish(down, json.dumps(msg)).wait_for_publish(10)
    sent = time.time()
    print(f"{time.strftime('%T')} MQTT  sent; watching {up} for up to {a.timeout} s (Ctrl-C stops watching only)", flush=True)
    try:
        while time.time() - sent < a.timeout and seen["after"] is None:
            time.sleep(1)
    except KeyboardInterrupt:
        print("stopped watching; the monitor carries on by itself")
    c.loop_stop()
    if server:
        server.shutdown()
    before, after = seen["before"], seen["after"]
    if local and UNDELIVERED and not DOWNLOADED:
        print("the download did not complete (see the HTTP lines above): the monitor stops at 99% and keeps "
              "running its current firmware; push again")
    elif after and before and after != before:
        print(f"done: the monitor rebooted and reports firmware {after} (was {before})")
    elif after and after == before:
        print(f"the monitor is still running {after}: the new image did not take effect "
              "(see the OTA status and HTTP lines above)")
    elif after:
        print(f"the monitor reports firmware {after}, with no earlier hello to compare: if that is what it was "
              "already running, the new image did not take effect (an ESPHome image never sends this hello)")
    elif local and not DOWNLOADED:
        print("the monitor never downloaded the file: check the type 24 status above, and that this PC's "
              f"firewall allows TCP {a.http_port} from the monitor")
    else:
        print("no hello from the new firmware yet; give it a few more minutes, then check the monitor's screen")

if __name__ == "__main__":
    main()
