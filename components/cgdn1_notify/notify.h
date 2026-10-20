// notify.h - MQTT notifications and questions on the CGDN1 screen (ours, not stock).
//
// Pure C++ (no ESPHome includes) so the host tests (tests/test_notify.py) compile it with the
// system compiler. The ESPHome glue (MQTT topics + JSON parsing, the touch slider, the display
// lambda, display wake) is in cgdn1.yaml.
//
// A message is a text (+ optional title) shown full screen for `timeout` seconds. Without
// actions it is a notification: any gesture dismisses it. With actions (gesture -> label) it is
// a question: only a mapped gesture answers it, every other one is swallowed. A message with an
// `id` reports how it left the screen (Outcome): the gesture that answered it, "dismissed",
// "timeout", "cleared" or "dropped". The timer starts when the message is first drawn (not while
// the panel is dark or the boot screen is up). One message is shown, up to MAX_WAITING more
// wait in arrival order; a message with the id of one already queued replaces it in place.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace cgdn1_notify {

// The gestures an action can use (cgdn1_touch names; long_touch, 7.2 s still, is left out).
enum Gesture : uint8_t { TAP_LEFT, TAP_CENTRE, TAP_RIGHT, SWIPE_LEFT, SWIPE_RIGHT, GESTURE_COUNT };
static const char *const GESTURE_NAMES[GESTURE_COUNT] = {"tap_left", "tap_centre", "tap_right", "swipe_left",
                                                         "swipe_right"};

inline int gesture_index(const std::string &name) {
  for (int g = 0; g < GESTURE_COUNT; g++)
    if (name == GESTURE_NAMES[g])
      return g;
  return -1;
}

static constexpr int32_t DEFAULT_NOTE_S = 10;      // notification on screen
static constexpr int32_t DEFAULT_QUESTION_S = 60;  // question waiting for an answer
static constexpr int32_t MAX_TIMEOUT_S = 3600;     // OLED: nothing stays up for good (burn-in)
static constexpr size_t MAX_WAITING = 4;           // behind the shown one
static constexpr size_t MAX_ID = 64, MAX_TITLE = 64, MAX_TEXT = 512, MAX_LABEL = 32;  // bytes

// Cuts s to at most n bytes without splitting a UTF-8 character.
inline void utf8_cut(std::string &s, size_t n) {
  if (s.size() <= n)
    return;
  while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80)
    n--;
  s.resize(n);
}

// Drops the last UTF-8 character of s.
inline void utf8_pop(std::string &s) {
  while (!s.empty() && (static_cast<uint8_t>(s.back()) & 0xC0) == 0x80)
    s.pop_back();
  if (!s.empty())
    s.pop_back();
}

struct Message {
  std::string id, title, text;
  std::string labels[GESTURE_COUNT];  // empty = gesture not mapped
  int32_t timeout_s{0};               // <= 0: the default for the kind
  bool wake{true};                    // light a dark panel for it

  bool question() const {
    for (const auto &l : this->labels)
      if (!l.empty())
        return true;
    return false;
  }
};

// How a message with an id left the screen: response = the answering gesture's name, or
// dismissed / timeout / cleared / dropped; label = the answering action's label.
struct Outcome {
  std::string id, response, label;

  std::string json() const {
    std::string s = "{\"id\":";
    append_json_string(s, this->id);
    s += ",\"response\":";
    append_json_string(s, this->response);
    if (!this->label.empty()) {
      s += ",\"label\":";
      append_json_string(s, this->label);
    }
    s += '}';
    return s;
  }

  static void append_json_string(std::string &out, const std::string &v) {
    static const char HEX[] = "0123456789abcdef";
    out += '"';
    for (char c : v) {
      const uint8_t u = static_cast<uint8_t>(c);
      if (c == '"' || c == '\\') {
        out += '\\';
        out += c;
      } else if (u < 0x20) {
        out += "\\u00";
        out += HEX[u >> 4];
        out += HEX[u & 0xF];
      } else {
        out += c;
      }
    }
    out += '"';
  }
};

class Queue {
 public:
  /// Takes a parsed message: trims the fields to their limits, fills the default timeout. A
  /// message without text is refused (false).
  bool push(Message m) {
    utf8_cut(m.id, MAX_ID);
    utf8_cut(m.title, MAX_TITLE);
    utf8_cut(m.text, MAX_TEXT);
    for (auto &l : m.labels)
      utf8_cut(l, MAX_LABEL);
    if (m.text.empty())
      return false;
    if (m.timeout_s <= 0)
      m.timeout_s = m.question() ? DEFAULT_QUESTION_S : DEFAULT_NOTE_S;
    if (m.timeout_s > MAX_TIMEOUT_S)
      m.timeout_s = MAX_TIMEOUT_S;
    if (!m.id.empty()) {
      for (size_t i = 0; i < this->q_.size(); i++) {
        if (this->q_[i].m.id != m.id)
          continue;
        this->q_[i] = Entry{std::move(m)};  // same id: new content, timer restarts when drawn
        if (i == 0)
          this->new_current_ = true;
        return true;
      }
    }
    this->q_.push_back(Entry{std::move(m)});
    if (this->q_.size() == 1)
      this->new_current_ = true;
    if (this->q_.size() > MAX_WAITING + 1) {
      this->report_(this->q_[1].m, "dropped");  // the oldest waiting one
      this->q_.erase(this->q_.begin() + 1);
    }
    return true;
  }

  bool active() const { return !this->q_.empty(); }
  size_t size() const { return this->q_.size(); }
  /// The shown message (only while active()).
  const Message &current() const { return this->q_.front().m; }

  /// The display drew the current message: starts its timer the first time.
  void shown(uint32_t now) {
    if (this->q_.empty() || this->q_.front().started)
      return;
    this->q_.front().started = true;
    this->q_.front().t0 = now;
  }

  /// A gesture while active(): answers / dismisses the current message or is swallowed.
  void gesture(const std::string &name) {
    if (this->q_.empty())
      return;
    const Message &m = this->q_.front().m;
    if (!m.question()) {
      this->finish_("dismissed", "");
      return;
    }
    const int g = gesture_index(name);
    if (g >= 0 && !m.labels[g].empty())
      this->finish_(GESTURE_NAMES[g], m.labels[g]);
  }

  /// Ends the current message once its timer has run out.
  void tick(uint32_t now) {
    if (this->q_.empty() || !this->q_.front().started)
      return;
    if (now - this->q_.front().t0 >= static_cast<uint32_t>(this->q_.front().m.timeout_s) * 1000u)
      this->finish_("timeout", "");
  }

  /// Removes the message with this id (shown or waiting), or every message for "".
  void clear(const std::string &id) {
    for (size_t i = this->q_.size(); i-- > 0;) {
      if (!id.empty() && this->q_[i].m.id != id)
        continue;
      this->report_(this->q_[i].m, "cleared");
      this->q_.erase(this->q_.begin() + i);
      if (i == 0 && !this->q_.empty())
        this->new_current_ = true;
    }
  }

  /// Share of the current message's time left: 1 before it is drawn, down to 0.
  float remaining(uint32_t now) const {
    if (this->q_.empty() || !this->q_.front().started)
      return 1.0f;
    const float total = this->q_.front().m.timeout_s * 1000.0f, gone = now - this->q_.front().t0;
    return gone >= total ? 0.0f : 1.0f - gone / total;
  }

  /// The outcomes since the last call, oldest first (to publish).
  std::vector<Outcome> take_outcomes() {
    std::vector<Outcome> out;
    out.swap(this->out_);
    return out;
  }

  /// True once each time a message became the shown one (the glue lights the panel for it).
  bool take_new_current() {
    const bool n = this->new_current_;
    this->new_current_ = false;
    return n && !this->q_.empty();
  }

 protected:
  struct Entry {
    Message m;
    uint32_t t0{0};
    bool started{false};
  };

  void report_(const Message &m, const char *response, const std::string &label = "") {
    if (!m.id.empty())
      this->out_.push_back(Outcome{m.id, response, label});
  }

  void finish_(const char *response, const std::string &label) {
    this->report_(this->q_.front().m, response, label);
    this->q_.pop_front();
    if (!this->q_.empty())
      this->new_current_ = true;
  }

  std::deque<Entry> q_;
  std::vector<Outcome> out_;
  bool new_current_{false};
};

using Measure = std::function<int(const std::string &)>;

// Cuts s (dropping whole UTF-8 characters) until s + ".." fits in width, then appends "..".
inline std::string ellipsize(std::string s, int width, const Measure &measure) {
  while (!s.empty() && measure(s + "..") > width)
    utf8_pop(s);
  return s + "..";
}

// The text as at most max_lines lines no wider than width (measure = pixel width of a string).
// Breaks at spaces, '\n' starts a new line, a word wider than a line is split between UTF-8
// characters. Text that needs more lines ends the last one with ".." (and sets *truncated).
inline std::vector<std::string> wrap(std::string text, int width, int max_lines, const Measure &measure,
                                     bool *truncated = nullptr) {
  std::vector<std::string> lines;
  if (truncated != nullptr)
    *truncated = max_lines <= 0;
  if (max_lines <= 0)
    return lines;
  while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
    text.pop_back();  // no blank lines at the end
  bool cut = false;
  auto emit = [&](const std::string &l) {
    if (static_cast<int>(lines.size()) == max_lines) {
      cut = true;
      return false;
    }
    lines.push_back(l);
    return true;
  };
  size_t p = 0;
  while (p <= text.size() && !cut) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos)
      e = text.size();
    const std::string para = text.substr(p, e - p);
    p = e + 1;
    std::string cur;
    size_t w = 0;
    bool full = false;
    while (w < para.size() && !full) {
      if (para[w] == ' ') {
        w++;
        continue;
      }
      size_t we = para.find(' ', w);
      if (we == std::string::npos)
        we = para.size();
      std::string word = para.substr(w, we - w);
      w = we;
      const std::string cand = cur.empty() ? word : cur + " " + word;
      if (measure(cand) <= width) {
        cur = cand;
        continue;
      }
      if (!cur.empty() && !emit(cur)) {
        full = true;
        break;
      }
      cur.clear();
      while (measure(word) > width) {  // a word longer than a line: split it
        size_t n = 0;
        for (size_t k = 1; k <= word.size(); k++) {
          if (k < word.size() && (static_cast<uint8_t>(word[k]) & 0xC0) == 0x80)
            continue;  // inside a character
          if (measure(word.substr(0, k)) > width)
            break;
          n = k;
        }
        if (n == 0) {  // not even one character fits
          n = 1;
          while (n < word.size() && (static_cast<uint8_t>(word[n]) & 0xC0) == 0x80)
            n++;
        }
        if (!emit(word.substr(0, n))) {
          full = true;
          break;
        }
        word.erase(0, n);
      }
      cur = word;
    }
    if (!full)
      emit(cur);
    if (e == text.size())
      break;
  }
  if (cut && !lines.empty())
    lines.back() = ellipsize(lines.back(), width, measure);
  if (truncated != nullptr)
    *truncated = cut;
  return lines;
}

// The hint row of a question: what each part of the slider does. Left = tap_left and/or
// "<< " swipe_left, centre = tap_centre, right = tap_right and/or swipe_right " >>" (a swipe
// toward the left end is swipe_left).
struct Hints {
  std::string left, centre, right;
  bool empty() const { return this->left.empty() && this->centre.empty() && this->right.empty(); }
};

inline Hints hints(const Message &m) {
  Hints h;
  auto join = [](const std::string &a, const std::string &b) { return a.empty() ? b : b.empty() ? a : a + " " + b; };
  const std::string &sl = m.labels[SWIPE_LEFT], &sr = m.labels[SWIPE_RIGHT];
  h.left = join(m.labels[TAP_LEFT], sl.empty() ? "" : "<< " + sl);
  h.centre = m.labels[TAP_CENTRE];
  h.right = join(sr.empty() ? "" : sr + " >>", m.labels[TAP_RIGHT]);
  return h;
}

}  // namespace cgdn1_notify
