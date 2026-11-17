"""Merged ESPHome config per build option (`esphome -s ... config`), checked key by key.

A config that validates can still be wrong: a package can leave a component in, or drop the
timeout that takes a trial build back to stock. Needs secrets.yaml (CI writes throwaway ones).
"""
import subprocess
import sys
from functools import lru_cache
from pathlib import Path

import pytest
import yaml

from conftest import require

ROOT = Path(__file__).resolve().parent.parent


class _Loader(yaml.SafeLoader):
    pass


def _untagged(loader, suffix, node):
    if isinstance(node, yaml.ScalarNode):
        return loader.construct_scalar(node)
    if isinstance(node, yaml.SequenceNode):
        return loader.construct_sequence(node, deep=True)
    return loader.construct_mapping(node, deep=True)


_Loader.add_multi_constructor("!", _untagged)  # !secret, !lambda: keep the plain value


@lru_cache(maxsize=None)
def merged(*subs):
    """Merged config for `-s k v` pairs, e.g. merged("trial_mode", "true")."""
    require((ROOT / "secrets.yaml").exists(), "secrets.yaml absent; copy secrets.yaml.example")
    args = [sys.executable, "-m", "esphome"]
    for k, v in zip(subs[::2], subs[1::2]):
        args += ["-s", k, v]
    res = subprocess.run(args + ["config", "cgdn1.yaml"], cwd=ROOT, capture_output=True, text=True)
    assert res.returncode == 0, f"esphome {' '.join(args[3:])} config failed:\n{res.stderr[-3000:]}"
    return yaml.load(res.stdout, Loader=_Loader)


def lambdas(cfg, key):
    return "\n".join(str(e.get("then")) for e in cfg.get(key, []))


DEFAULT = ()
TRIAL = ("trial_mode", "true")
ALL_ON = ("trial_mode", "true", "bthome", "true", "mqtt_messages", "true")


def test_default_build():
    cfg = merged(*DEFAULT)
    assert cfg["cgdn1_boot"]["trial_mode"] in (False, "false")
    assert "ap" in cfg["wifi"] and "captive_portal" in cfg
    assert cfg["api"]["reboot_timeout"] == "15min"
    assert [t["platform"] for t in cfg["time"]] == ["homeassistant"]
    assert "mqtt" not in cfg
    assert "esp_ble_gap_config_adv_data_raw" not in lambdas(cfg, "interval")


def test_bluetooth_proxy_stays_passive():
    # Active connections asserted the BT controller blob after ~2 h (cgdn1.yaml comment).
    for subs in (DEFAULT, ALL_ON):
        cfg = merged(*subs)
        assert "esp32_ble_tracker" in cfg
        assert cfg["bluetooth_proxy"]["active"] in (False, "false")


@pytest.mark.parametrize("subs", [TRIAL, ALL_ON], ids=["trial", "all_on"])
def test_trial_build_can_return_to_stock(subs):
    cfg = merged(*subs)
    assert cfg["cgdn1_boot"]["trial_mode"] in (True, "true")
    # 15 min without an API client reboots, and in trial mode every reboot boots stock.
    assert cfg["api"]["reboot_timeout"] == "15min"


def test_bthome_broadcasts():
    cfg = merged(*ALL_ON)
    assert "esp_ble_gap_config_adv_data_raw" in lambdas(cfg, "interval")


def test_mqtt_messages_never_reboot_or_discover():
    mqtt = merged(*ALL_ON)["mqtt"]
    # Screen messages only: a missing broker must not reboot the unit (in trial: back to stock),
    # and HA already has every entity over the API.
    assert mqtt["reboot_timeout"] == "0s"
    assert mqtt["discovery"] in (False, "false")
