"""Host tests for the CGDN1 MQTT notification core (components/cgdn1_notify/notify.h).

A tiny C++ driver is compiled against the header with the system C++ compiler (skipped when none
is installed) and fed commands over stdin. Messages travel as '|'-separated fields:
id|title|text|timeout|wake|tap_left|tap_centre|tap_right|swipe_left|swipe_right. The word-wrap
measure is 6 px per UTF-8 character.
"""
import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
NOTIFY_DIR = ROOT / "components" / "cgdn1_notify"

DRIVER = r"""
#include "notify.h"
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
using namespace cgdn1_notify;
static std::vector<std::string> split(const std::string &s, char d) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : s) { if (c == d) { v.push_back(cur); cur.clear(); } else cur += c; }
  v.push_back(cur);
  return v;
}
static int measure(const std::string &s) {
  int n = 0;
  for (char c : s) if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) n++;
  return 6 * n;
}
static std::string rest(std::istringstream &in) {
  std::string r; std::getline(in, r);
  if (!r.empty() && r[0] == ' ') r.erase(0, 1);
  for (auto &c : r) if (c == '~') c = '\n';  // '~' = a newline in the text
  return r;
}
static Message parse(const std::string &s) {
  auto f = split(s, '|');
  f.resize(10);
  Message m;
  m.id = f[0]; m.title = f[1]; m.text = f[2];
  m.timeout_s = f[3].empty() ? 0 : std::stoi(f[3]);
  m.wake = f[4] != "0";
  for (int g = 0; g < GESTURE_COUNT; g++) m.labels[g] = f[5 + g];
  return m;
}
int main() {
  Queue q;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "P") printf("%s\n", q.push(parse(rest(in))) ? "ok" : "refused");
    else if (cmd == "S") { uint32_t t; in >> t; q.shown(t); printf("ok\n"); }
    else if (cmd == "G") { std::string g; in >> g; q.gesture(g); printf("ok\n"); }
    else if (cmd == "T") { uint32_t t; in >> t; q.tick(t); printf("ok\n"); }
    else if (cmd == "C") { q.clear(rest(in)); printf("ok\n"); }
    else if (cmd == "R") { uint32_t t; in >> t; printf("%.3f\n", q.remaining(t)); }
    else if (cmd == "N") printf("%d\n", (int) q.take_new_current());
    else if (cmd == "Q") {
      if (!q.active()) printf("-\n");
      else printf("%s|%s|%s|%d|%d|%d\n", q.current().id.c_str(), q.current().title.c_str(), q.current().text.c_str(),
                  (int) q.current().timeout_s, (int) q.current().wake, (int) q.size());
    } else if (cmd == "O") {
      std::string s = "[";
      for (auto &o : q.take_outcomes()) { if (s.size() > 1) s += ","; s += o.json(); }
      printf("%s]\n", s.c_str());
    } else if (cmd == "W") {
      int w, n; in >> w >> n;
      bool cut = true;
      auto lines = wrap(rest(in), w, n, measure, &cut);
      std::string s;
      for (size_t i = 0; i < lines.size(); i++) { if (i) s += "|"; s += lines[i]; }
      printf("%zu%s:%s\n", lines.size(), cut ? "+" : "", s.c_str());
    } else if (cmd == "H") {
      Hints h = hints(parse("|||||" + rest(in)));
      printf("%s|%s|%s\n", h.left.c_str(), h.centre.c_str(), h.right.c_str());
    }
  }
}
"""


@pytest.fixture(scope="module")
def drv(tmp_path_factory):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("notify")
    src = d / "drv.cpp"
    src.write_text(DRIVER)
    exe = d / "drv"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(NOTIFY_DIR), str(src), "-o", str(exe)],
                   check=True)
    return exe


def run(exe, lines):
    out = subprocess.run([str(exe)], input="".join(l + "\n" for l in lines), capture_output=True, text=True,
                         check=True).stdout
    return out.splitlines()


def msg(id="", title="", text="hello", timeout="", wake=1, tl="", tc="", tr="", sl="", sr=""):
    return f"P {id}|{title}|{text}|{timeout}|{wake}|{tl}|{tc}|{tr}|{sl}|{sr}"


def outcomes(line):
    return json.loads(line)


# ---- queue --------------------------------------------------------------------------------------

def test_notification_defaults_and_timeout(drv):
    got = run(drv, [msg(id="n1"), "N", "Q", "T 5000", "S 1000", "R 6000", "T 10999", "Q", "T 11000", "Q", "O"])
    assert got[:3] == ["ok", "1", "n1||hello|10|1|1"]
    assert got[5] == "0.500"  # 5 of 10 s gone
    assert got[7] == "n1||hello|10|1|1"  # timer starts at the first draw (1000), not at push
    assert got[9] == "-"
    assert outcomes(got[10]) == [{"id": "n1", "response": "timeout"}]


def test_question_default_timeout_and_answer(drv):
    got = run(drv, [msg(id="door", text="Lock?", tl="Yes", tr="No"), "Q", "S 0",
                    "G tap_centre", "G swipe_left", "Q", "G tap_left", "Q", "O"])
    assert got[1] == "door||Lock?|60|1|1"
    assert got[5] == "door||Lock?|60|1|1"  # unmapped gestures are swallowed
    assert got[7] == "-"
    assert outcomes(got[8]) == [{"id": "door", "response": "tap_left", "label": "Yes"}]


def test_notification_any_gesture_dismisses(drv):
    got = run(drv, [msg(id="a"), "S 0", "G swipe_right", "Q", "O"])
    assert got[3] == "-"
    assert outcomes(got[4]) == [{"id": "a", "response": "dismissed"}]


def test_no_id_no_outcome(drv):
    got = run(drv, [msg(), msg(tl="ok"), "G tap_left", "G tap_left", "Q", "O"])
    assert got[4] == "-"
    assert got[5] == "[]"


def test_empty_text_refused_and_limits(drv):
    long = "x" * 600
    got = run(drv, [msg(text=""), msg(id="i" * 80, text=long, timeout=99999), "Q"])
    assert got[0] == "refused"
    i, _, text, timeout, _, _ = got[2].split("|")
    assert len(i) == 64 and len(text) == 512 and timeout == "3600"


def test_utf8_trim_keeps_whole_characters(drv):
    got = run(drv, [msg(text="я" * 300), "Q"])  # 600 bytes -> cut at 512 = 256 whole letters
    assert got[1].split("|")[2] == "я" * 256


def test_fifo_and_new_current(drv):
    got = run(drv, [msg(id="a"), "N", msg(id="b"), "N", "S 0", "G tap_left", "N", "Q", "O"])
    assert got[1] == "1" and got[3] == "0"
    assert got[6] == "1"  # b became the shown one
    assert got[7].startswith("b|")
    assert outcomes(got[8]) == [{"id": "a", "response": "dismissed"}]


def test_queue_full_drops_oldest_waiting(drv):
    got = run(drv, [msg(id=c) for c in "abcdef"] + ["Q", "O"])
    assert got[6].endswith("|5")  # a shown + 4 waiting
    assert got[6].startswith("a|")
    assert outcomes(got[7]) == [{"id": "b", "response": "dropped"}]


def test_same_id_replaces_in_place(drv):
    got = run(drv, [msg(id="a", text="one"), msg(id="b"), "N", "S 0", "T 9000",
                    msg(id="a", text="two"), "N", "Q", "T 12000", "Q", "T 21000", "Q", "O"])
    assert got[2] == "1"
    assert got[6] == "1"  # the shown one changed: light the panel again
    assert got[7] == "a||two|10|1|2"
    assert got[9].startswith("a|")  # timer restarted, not yet drawn again
    assert got[11].startswith("a|")
    assert got[12] == "[]"


def test_clear_by_id_and_all(drv):
    got = run(drv, [msg(id="a"), msg(id="b"), msg(), "C b", "Q", "C a", "Q", "N", "C", "Q", "O"])
    assert got[4].startswith("a|") and got[4].endswith("|2")
    assert got[6].startswith("||hello")
    assert got[9] == "-"
    assert outcomes(got[10]) == [{"id": "b", "response": "cleared"}, {"id": "a", "response": "cleared"}]


def test_millis_wrap(drv):
    got = run(drv, [msg(id="w"), "S 4294962296", "T 4294967295", "Q", "T 4999", "Q", "T 5000", "Q"])
    assert got[3].startswith("w|") and got[5].startswith("w|") and got[7] == "-"


def test_json_escaping(drv):
    got = run(drv, [msg(id='q"\\x', tl='a"b'), "G tap_left", "O"])
    assert outcomes(got[2]) == [{"id": 'q"\\x', "response": "tap_left", "label": 'a"b'}]


def test_wake_flag(drv):
    got = run(drv, [msg(wake=0), "Q"])
    assert got[1].split("|")[4] == "0"


# ---- word wrap (6 px / character) ---------------------------------------------------------------

def test_wrap_words(drv):
    got = run(drv, ["W 60 5 the quick brown fox jumps over"])  # 10 characters per line
    assert got == ["3:the quick|brown fox|jumps over"]


def test_wrap_newline_and_spaces(drv):
    got = run(drv, ["W 60 5 a  b~~c~ ~"])
    assert got == ["3:a b||c"]


def test_wrap_long_word_split(drv):
    got = run(drv, ["W 30 5 abcdefghijkl xy"])  # 5 per line
    assert got == ["3:abcde|fghij|kl xy"]


def test_wrap_overflow_ellipsis(drv):
    got = run(drv, ["W 60 2 one two three four five six seven"])
    assert got == ["2+:one two|three fo.."]


def test_wrap_utf8(drv):
    got = run(drv, ["W 30 2 привітання світ"])
    assert got == ["2+:приві|тан.."]


# ---- hint row -----------------------------------------------------------------------------------

def test_hints(drv):
    got = run(drv, ["H Yes||No||", "H |OK|||", "H ||Skip|Back|Next", "H A||||B"])
    assert got == ["Yes||No", "|OK|", "<< Back||Next >> Skip", "A||B >>"]
