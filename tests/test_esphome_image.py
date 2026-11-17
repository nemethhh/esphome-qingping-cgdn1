"""Pin the ESPHome beachhead image + partition table to the stock CGDN1 layout.

Partition CSV tests always run; build tests check the last `esphome [-s ...] compile cgdn1.yaml`
(CI builds each variant in turn) and skip without one, except in CI, where they fail.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from cgdn1_ota_push import check_esp_image
from conftest import require

ROOT = Path(__file__).resolve().parent.parent
ESPHOME_DIR = ROOT
CSV = ESPHOME_DIR / "components" / "cgdn1_boot" / "cgdn1-partitions.csv"
BUILD = ESPHOME_DIR / ".esphome" / "build" / "cgdn1"
IMAGE = BUILD / "build" / "firmware.ota.bin"
SDKCONFIG = BUILD / "sdkconfig.cgdn1"
BUILT_CSV = BUILD / "partitions.csv"
SLOT_SIZE = 0x1E0000

STOCK = {
    "nvs": ("data", "nvs", 0x9000, 0x4000),
    "otadata": ("data", "ota", 0xD000, 0x2000),
    "phy_init": ("data", "phy", 0xF000, 0x1000),
    "ota_0": ("app", "ota_0", 0x10000, 0x1E0000),
    "ota_1": ("app", "ota_1", 0x1F0000, 0x1E0000),
    "test": ("app", "test", 0x3D0000, 0x23000),
    "factory_nvs": ("data", "nvs", 0x3F3000, 0x4000),
    "nvs_keys": ("data", "nvs_keys", 0x3F7000, 0x1000),
    "minvs": ("data", "0xfe", 0x3F8000, 0x4000),
}


def parse_csv(path=CSV):
    rows = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, typ, sub, off, size = [f.strip() for f in line.split(",")[:5]]
        rows[name] = (typ, sub, int(off, 0), int(size, 0))
    return rows


def test_partition_table_matches_stock():
    assert parse_csv() == STOCK


def test_ota_0_fits_slot():
    typ, sub, off, size = parse_csv()["ota_0"]
    assert size <= SLOT_SIZE


@pytest.fixture(scope="module")
def image():
    require(IMAGE.exists(), f"build output absent ({IMAGE}); run: esphome compile cgdn1.yaml")
    return IMAGE


def test_build_used_stock_partition_table():
    require(BUILT_CSV.exists(), f"build output absent ({BUILT_CSV})")
    assert parse_csv(BUILT_CSV) == STOCK


def sdkconfig():
    require(SDKCONFIG.exists(), f"build output absent ({SDKCONFIG})")
    opts = {}
    for line in SDKCONFIG.read_text().splitlines():
        if m := re.fullmatch(r"(CONFIG_\w+)=(.*)", line):
            opts[m[1]] = m[2]
        elif m := re.fullmatch(r"# (CONFIG_\w+) is not set", line):
            opts[m[1]] = "n"
    return opts


@pytest.mark.parametrize("key,want", [
    # The stock 4.0.3 bootloader has no rollback: an image that expects it is never confirmed.
    ("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE", "n"),
    # NVS still holds stock's WiFi/MQTT config (stock stays in the other slot).
    ("CONFIG_ESP_WIFI_NVS_ENABLED", "n"),
    ("CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE", "n"),
    ("CONFIG_ESPTOOLPY_FLASHMODE_DIO", "y"),
    ("CONFIG_ESPTOOLPY_FLASHSIZE_4MB", "y"),
    ("CONFIG_PARTITION_TABLE_CUSTOM", "y"),
])
def test_sdkconfig_keeps_stock_safe(key, want):
    assert sdkconfig().get(key, "n") == want


def test_image_fits_ota_0(image):
    assert image.stat().st_size <= SLOT_SIZE


def test_image_structure(image):
    facts = check_esp_image(image.read_bytes())  # validates chip id 0, checksum, appended SHA-256
    assert facts["segments"] <= 8, "9+ segment images hang the stock OTA commit-verify at 99%"
    data = image.read_bytes()
    assert data[12:14] == b"\x00\x00", "chip id must be 0 (ESP32)"
    assert data[23] == 1, "hash-appended byte must be set"


def test_image_header_via_esptool(image):
    require(shutil.which("esptool"), "esptool not on PATH")
    out = subprocess.run(["esptool", "image-info", str(image)], capture_output=True, text=True, check=True).stdout
    assert "Detected image type: ESP32" in out
    assert re.search(r"Flash mode:\s*DIO", out)
    assert re.search(r"Flash freq:\s*80m", out)
    assert re.search(r"Flash size:\s*4MB", out)
    assert re.search(r"Validation hash: [0-9a-f]{64} \(valid\)", out)


def _segments(data):
    """(load_addr, length) per image segment; header is 24 bytes, each segment has an 8-byte header."""
    n = data[1]
    off = 24
    segs = []
    for _ in range(n):
        addr = int.from_bytes(data[off:off + 4], "little")
        length = int.from_bytes(data[off + 4:off + 8], "little")
        segs.append((addr, length))
        off += 8 + length
    return segs


def test_load_regions_safe_for_old_bootloader(image):
    """The 4.0.3 bootloader loads RAM segments itself; overlap with its own area would crash it."""
    segs = _segments(image.read_bytes())
    assert segs
    for addr, length in segs:
        end = addr + length
        if 0x40080000 <= addr <= 0x400BFFFF:
            assert end <= 0x400A0000, f"IRAM segment {addr:#x}+{length:#x} ends past 0x400A0000"
        if 0x3FFAE000 <= addr <= 0x3FFFFFFF:
            assert end < 0x3FFE0000, f"DRAM segment {addr:#x}+{length:#x} reaches 0x3FFE0000"


def test_yaml_does_not_enable_sram1_as_iram():
    assert "sram1_as_iram" not in (ESPHOME_DIR / "cgdn1.yaml").read_text().lower()
