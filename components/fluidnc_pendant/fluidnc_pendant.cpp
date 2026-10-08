#include "fluidnc_pendant.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fluidnc_pendant {

static const char *const TAG = "fluidnc_pendant";

static std::vector<uint8_t> hex_to_bytes(const std::string &hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    char pair[3] = {hex[i], hex[i + 1], 0};
    out.push_back((uint8_t) strtoul(pair, nullptr, 16));
  }
  return out;
}

void FluidNCPendant::add_button(InternalGPIOPin *pin, const std::string &name, const std::string &press_hex,
                                const std::string &release_hex, const std::string &press_alt_hex,
                                uint16_t alt_mask, uint16_t require_mask, uint32_t hold_ms, uint32_t repeat_ms,
                                uint32_t lockout_ms) {
  Button b;
  b.pin = pin;
  b.name = name;
  b.press = hex_to_bytes(press_hex);
  b.release = hex_to_bytes(release_hex);
  b.press_alt = hex_to_bytes(press_alt_hex);
  b.alt_mask = alt_mask;
  b.require_mask = require_mask;
  b.hold_ms = hold_ms;
  b.repeat_ms = repeat_ms;
  b.lockout_ms = lockout_ms;
  this->buttons_.push_back(b);
}

const char *FluidNCPendant::state_name_(MachState s) const {
  switch (s) {
    case ST_IDLE: return "Idle";
    case ST_RUN: return "Run";
    case ST_HOLD: return "Hold";
    case ST_JOG: return "Jog";
    case ST_ALARM: return "Alarm";
    case ST_DOOR: return "Door";
    case ST_CHECK: return "Check";
    case ST_HOME: return "Home";
    case ST_SLEEP: return "Sleep";
    case ST_OTHER: return "Other";
    default: return "Unknown";
  }
}

std::string FluidNCPendant::mask_names_(uint16_t mask) const {
  std::string out;
  for (uint8_t s = ST_IDLE; s <= ST_SLEEP; s++) {
    if (mask & (1u << s)) {
      if (!out.empty()) out += "/";
      out += this->state_name_((MachState) s);
    }
  }
  return out;
}

std::string FluidNCPendant::pretty_(const std::vector<uint8_t> &bytes) const {
  // Printable ASCII as-is, newline as \n, anything else as <0xNN>, so the exact bytes are visible
  std::string out;
  char tmp[8];
  for (uint8_t c : bytes) {
    if (c == '\n') {
      out += "\\n";
    } else if (c >= 32 && c < 127) {
      out += (char) c;
    } else {
      snprintf(tmp, sizeof(tmp), "<0x%02X>", c);
      out += tmp;
    }
  }
  return out;
}

bool FluidNCPendant::status_fresh_() const {
  return this->state_ != ST_UNKNOWN && (millis() - this->last_status_ms_) < this->status_timeout_ms_;
}

void FluidNCPendant::setup() {
  uint32_t now = millis();
  for (auto &b : this->buttons_) {
    b.pin->setup();
    bool pressed = !b.pin->digital_read();  // wired to GND: pressed = LOW
    b.stable = b.last_raw = pressed;
    b.ignore_until_release = pressed;
    b.last_change = now;
  }
}

void FluidNCPendant::dump_config() {
  ESP_LOGCONFIG(TAG, "FluidNC pendant:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->dry_run_ ? "DRY-RUN (nothing is sent for buttons)" : "LIVE");
  ESP_LOGCONFIG(TAG, "  Status poll: %ums, timeout: %ums, debounce: %ums", (unsigned) this->status_poll_ms_,
                (unsigned) this->status_timeout_ms_, (unsigned) this->debounce_ms_);
  uint8_t n = 0;
  for (auto &b : this->buttons_) {
    n++;
    ESP_LOGCONFIG(TAG, "  Button No. %u  %-12s GPIO%u", n, b.name.c_str(), (unsigned) b.pin->get_pin());
    if (!b.press.empty()) {
      ESP_LOGCONFIG(TAG, "      on press  : \"%s\"", this->pretty_(b.press).c_str());
    }
    if (!b.press_alt.empty()) {
      ESP_LOGCONFIG(TAG, "      press alt : \"%s\" when %s", this->pretty_(b.press_alt).c_str(),
                    this->mask_names_(b.alt_mask).c_str());
    }
    if (!b.release.empty()) {
      ESP_LOGCONFIG(TAG, "      on release: \"%s\"", this->pretty_(b.release).c_str());
    }
    if (b.require_mask) {
      ESP_LOGCONFIG(TAG, "      only when : %s", this->mask_names_(b.require_mask).c_str());
    }
    if (b.hold_ms) {
      ESP_LOGCONFIG(TAG, "      hold time : %ums", (unsigned) b.hold_ms);
    }
    if (b.repeat_ms) {
      ESP_LOGCONFIG(TAG, "      repeat    : every %ums while held", (unsigned) b.repeat_ms);
    }
    if (b.lockout_ms) {
      ESP_LOGCONFIG(TAG, "      lockout   : %ums", (unsigned) b.lockout_ms);
    }
    if (b.ignore_until_release) {
      ESP_LOGW(TAG, "      LOW at boot: ignored until released (stuck or shorted?)");
    }
  }
}

// ---------------------------------------------------------------- transmit
void FluidNCPendant::tx_(const std::vector<uint8_t> &bytes, const char *what, bool log) {
  if (bytes.empty()) return;
  std::string shown = this->pretty_(bytes);
  if (this->dry_run_) {
    if (log) ESP_LOGI(TAG, "  -> UART TX [DRY-RUN, NOT sent]: \"%s\"   (%s)", shown.c_str(), what);
    return;
  }
  this->write_array(bytes.data(), bytes.size());
  for (uint8_t c : bytes)
    if (c == '\n') this->pending_ok_++;
  this->pending_ms_ = millis();
  if (log) ESP_LOGI(TAG, "  -> UART TX: \"%s\"   (%s)", shown.c_str(), what);
}

// ---------------------------------------------------------------- receive
void FluidNCPendant::parse_status_(const char *line) {
  const char *p = line + 1;
  char tok[16];
  size_t n = 0;
  while (p[n] && p[n] != '|' && p[n] != '>' && p[n] != ':' && n < sizeof(tok) - 1) {
    tok[n] = p[n];
    n++;
  }
  tok[n] = 0;
  MachState ns = ST_OTHER;
  if (!strcmp(tok, "Idle")) ns = ST_IDLE;
  else if (!strcmp(tok, "Run")) ns = ST_RUN;
  else if (!strcmp(tok, "Hold")) ns = ST_HOLD;
  else if (!strcmp(tok, "Jog")) ns = ST_JOG;
  else if (!strcmp(tok, "Alarm")) ns = ST_ALARM;
  else if (!strcmp(tok, "Door")) ns = ST_DOOR;
  else if (!strcmp(tok, "Check")) ns = ST_CHECK;
  else if (!strcmp(tok, "Home")) ns = ST_HOME;
  else if (!strcmp(tok, "Sleep")) ns = ST_SLEEP;
  if (strcmp(tok, this->raw_state_) != 0) {
    ESP_LOGI(TAG, "STATE  %s -> %s", this->raw_state_[0] ? this->raw_state_ : "(none)", tok);
    snprintf(this->raw_state_, sizeof(this->raw_state_), "%s", tok);
  }
  this->state_ = ns;
  this->last_status_ms_ = millis();
}

void FluidNCPendant::handle_line_(const char *line) {
  this->rx_lines_++;
  if (line[0] == '<') {
    this->parse_status_(line);
    if (this->log_rx_) ESP_LOGD(TAG, "RX  %s", line);
    return;
  }
  if (!strcmp(line, "ok")) {
    if (this->pending_ok_) this->pending_ok_--;
    this->pending_ms_ = millis();
    if (this->log_rx_) ESP_LOGD(TAG, "RX  ok");
    return;
  }
  if (!strncmp(line, "error:", 6)) {
    if (this->pending_ok_) this->pending_ok_--;
    this->pending_ms_ = millis();
  } else if (!strncmp(line, "ALARM:", 6)) {
    this->state_ = ST_ALARM;
    this->last_status_ms_ = millis();
  }
  ESP_LOGI(TAG, "RX  %s", line);
}

void FluidNCPendant::read_link_() {
  uint8_t c;
  while (this->available() && this->read_byte(&c)) {
    this->rx_bytes_++;
    this->last_rx_ms_ = millis();
    if (c == '\r') continue;
    if (c == '\n') {
      this->line_[this->line_len_] = 0;
      for (uint16_t i = 0; i < this->line_len_; i++)  // binary/realtime bytes shown as '.'
        if ((uint8_t) this->line_[i] < 32 || (uint8_t) this->line_[i] > 126) this->line_[i] = '.';
      if (this->line_len_) this->handle_line_(this->line_);
      this->line_len_ = 0;
    } else if (this->line_len_ < sizeof(this->line_) - 1) {
      this->line_[this->line_len_++] = (char) c;
    } else {
      this->line_len_ = 0;
    }
  }
}

// Bytes arrived but no newline followed: show hex so noise / wrong baud / wrong device is obvious
void FluidNCPendant::flush_partial_rx_() {
  if (!this->line_len_ || millis() - this->last_rx_ms_ < 500) return;
  char hex[3 * 24 + 1], asc[25];
  uint8_t n = this->line_len_ < 24 ? this->line_len_ : 24;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t b = (uint8_t) this->line_[i];
    snprintf(hex + 3 * i, 4, "%02X ", b);
    asc[i] = (b >= 32 && b < 127) ? (char) b : '.';
  }
  hex[3 * n] = 0;
  asc[n] = 0;
  ESP_LOGW(TAG, "RX  partial, no newline (%u bytes): %s |%s|  <- noise or wrong baud/device?",
           (unsigned) this->line_len_, hex, asc);
  this->line_len_ = 0;
}

// ---------------------------------------------------------------- gating + actions
std::string FluidNCPendant::block_reason_(const Button &b) const {
  if (this->dry_run_ || !b.require_mask) return "";  // dry-run bypasses state gating so the logic can be bench-tested
  if (!this->status_fresh_()) return "no fresh status from FluidNC";
  if (!(b.require_mask & (1u << this->state_)))
    return std::string("machine state is ") + this->state_name_(this->state_) + ", needs " +
           this->mask_names_(b.require_mask);
  return "";
}

bool FluidNCPendant::try_fire_(Button &b, bool first) {
  std::string why = this->block_reason_(b);
  if (!why.empty()) {
    if (first) ESP_LOGI(TAG, "  -> nothing sent: %s", why.c_str());
    return false;
  }
  uint32_t now = millis();
  if (first && b.lockout_ms && b.last_fire && (now - b.last_fire) < b.lockout_ms) {
    ESP_LOGI(TAG, "  -> nothing sent: lockout (%ums between presses)", (unsigned) b.lockout_ms);
    return false;
  }
  const std::vector<uint8_t> *cmd = &b.press;
  const char *what = first ? "press" : "repeat";
  if (first && !b.press_alt.empty() && this->status_fresh_() && (b.alt_mask & (1u << this->state_))) {
    cmd = &b.press_alt;
    what = "press (alternate)";
  }
  this->tx_(*cmd, what, first);
  b.fired = true;
  b.last_send = now;
  if (first) b.last_fire = now;
  return true;
}

void FluidNCPendant::on_press_(uint8_t idx) {
  Button &b = this->buttons_[idx];
  ESP_LOGI(TAG, "Button No. %u pressed   (%s, GPIO%u)", idx + 1, b.name.c_str(), (unsigned) b.pin->get_pin());
  b.fired = false;
  b.hold_armed = false;
  b.gate_stopped = false;
  if (b.press.empty()) {
    ESP_LOGI(TAG, "  -> nothing sent on press (command goes out on release)");
    return;
  }
  if (b.hold_ms > 0) {
    b.hold_armed = true;
    ESP_LOGI(TAG, "  -> nothing sent yet: keep holding %ums", (unsigned) b.hold_ms);
    return;
  }
  this->try_fire_(b, true);
}

void FluidNCPendant::on_release_(uint8_t idx, uint32_t held_ms) {
  Button &b = this->buttons_[idx];
  ESP_LOGI(TAG, "Button No. %u released  (%s, held %ums)", idx + 1, b.name.c_str(), (unsigned) held_ms);
  if (b.hold_armed) {
    b.hold_armed = false;
    ESP_LOGI(TAG, "  -> nothing sent: released before %ums", (unsigned) b.hold_ms);
  }
  if (!b.release.empty()) {
    if (b.press.empty() || b.fired) {
      this->tx_(b.release, "release");
    } else if (!b.gate_stopped) {
      ESP_LOGI(TAG, "  -> nothing sent: press command never went out, so no release command");
    }
  }
  b.fired = false;
}

// ---------------------------------------------------------------- main loop
void FluidNCPendant::loop() {
  uint32_t now = millis();

  this->read_link_();
  this->flush_partial_rx_();

  // Ask for status if FluidNC has not sent one recently (the "?" query is read-only, also sent in dry-run)
  if (now - this->last_status_ms_ > this->status_poll_ms_ * 2 && now - this->last_poll_ms_ > this->status_poll_ms_) {
    this->write_byte('?');
    this->last_poll_ms_ = now;
  }

  // stale flow-control counter
  if (this->pending_ok_ && now - this->pending_ms_ > 1500) this->pending_ok_ = 0;

  for (uint8_t i = 0; i < this->buttons_.size(); i++) {
    Button &b = this->buttons_[i];

    // debounce
    bool raw = !b.pin->digital_read();
    if (raw != b.last_raw) {
      b.last_raw = raw;
      b.last_change = now;
    } else if (raw != b.stable && (now - b.last_change) >= this->debounce_ms_) {
      b.stable = raw;
      if (raw) {
        b.pressed_at = now;
        if (b.ignore_until_release)
          ESP_LOGI(TAG, "Button No. %u pressed but ignored (was low at boot)", i + 1);
        else
          this->on_press_(i);
      } else {
        if (b.ignore_until_release) {
          b.ignore_until_release = false;
          ESP_LOGI(TAG, "Button No. %u released - now active", i + 1);
        } else {
          this->on_release_(i, now - b.pressed_at);
        }
      }
    }

    if (!b.stable || b.ignore_until_release) continue;

    // hold-to-confirm
    if (b.hold_armed && (now - b.pressed_at) >= b.hold_ms) {
      b.hold_armed = false;
      this->try_fire_(b, true);
    }

    // repeat while held (continuous jog)
    if (b.fired && b.repeat_ms && (now - b.last_send) >= b.repeat_ms) {
      std::string why = this->block_reason_(b);
      if (!why.empty()) {
        ESP_LOGI(TAG, "Button No. %u: stopping repeat (%s)", i + 1, why.c_str());
        if (!b.release.empty()) this->tx_(b.release, "release (repeat stopped)");
        b.fired = false;
        b.gate_stopped = true;  // release command already sent; the release edge must not complain
      } else if (this->dry_run_ || this->pending_ok_ < 2) {
        this->try_fire_(b, false);
      }
    }
  }

  // heartbeat
  if (now - this->last_hb_ms_ >= 5000) {
    this->last_hb_ms_ = now;
    ESP_LOGD(TAG, "HB   mode=%s link=%s state=%s rxBytes=%u rxLines=%u pending=%u", this->dry_run_ ? "DRY" : "LIVE",
             this->status_fresh_() ? "OK" : "NONE", this->state_name_(this->state_), (unsigned) this->rx_bytes_,
             (unsigned) this->rx_lines_, (unsigned) this->pending_ok_);
    if (this->rx_lines_ == 0 && now > 8000)
      ESP_LOGW(TAG, "     no complete line from FluidNC yet (rxBytes=%u): check TX/RX crossed, common GND, baud, "
                    "uart1/uart_channel1 in config.yaml", (unsigned) this->rx_bytes_);
  }
}

}  // namespace fluidnc_pendant
}  // namespace esphome
