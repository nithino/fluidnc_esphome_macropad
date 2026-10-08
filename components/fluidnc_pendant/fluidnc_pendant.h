#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace fluidnc_pendant {

// Order MUST match STATES in __init__.py (UNKNOWN = 0, then idle, run, ...)
enum MachState : uint8_t {
  ST_UNKNOWN = 0,
  ST_IDLE,
  ST_RUN,
  ST_HOLD,
  ST_JOG,
  ST_ALARM,
  ST_DOOR,
  ST_CHECK,
  ST_HOME,
  ST_SLEEP,
  ST_OTHER,
};

struct Button {
  InternalGPIOPin *pin{nullptr};
  std::string name;
  std::vector<uint8_t> press, release, press_alt;
  uint16_t alt_mask{0}, require_mask{0};
  uint32_t hold_ms{0}, repeat_ms{0}, lockout_ms{0};

  // runtime
  bool stable{false}, last_raw{false}, ignore_until_release{false};
  bool fired{false}, hold_armed{false}, gate_stopped{false};
  uint32_t last_change{0}, pressed_at{0}, last_send{0}, last_fire{0};
};

class FluidNCPendant : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_dry_run(bool v) { this->dry_run_ = v; }
  void set_log_rx(bool v) { this->log_rx_ = v; }
  void set_status_poll_ms(uint32_t v) { this->status_poll_ms_ = v; }
  void set_status_timeout_ms(uint32_t v) { this->status_timeout_ms_ = v; }
  void set_debounce_ms(uint32_t v) { this->debounce_ms_ = v; }
  void add_button(InternalGPIOPin *pin, const std::string &name, const std::string &press_hex,
                  const std::string &release_hex, const std::string &press_alt_hex, uint16_t alt_mask,
                  uint16_t require_mask, uint32_t hold_ms, uint32_t repeat_ms, uint32_t lockout_ms);

 protected:
  void read_link_();
  void handle_line_(const char *line);
  void parse_status_(const char *line);
  void flush_partial_rx_();
  bool status_fresh_() const;
  const char *state_name_(MachState s) const;
  std::string mask_names_(uint16_t mask) const;
  std::string block_reason_(const Button &b) const;
  std::string pretty_(const std::vector<uint8_t> &bytes) const;
  void tx_(const std::vector<uint8_t> &bytes, const char *what, bool log = true);
  bool try_fire_(Button &b, bool first);
  void on_press_(uint8_t idx);
  void on_release_(uint8_t idx, uint32_t held_ms);

  std::vector<Button> buttons_;
  bool dry_run_{false}, log_rx_{false};
  uint32_t status_poll_ms_{250}, status_timeout_ms_{1000}, debounce_ms_{25};

  MachState state_{ST_UNKNOWN};
  char raw_state_[16]{};
  uint32_t last_status_ms_{0}, last_poll_ms_{0}, last_rx_ms_{0}, last_hb_ms_{0};
  uint32_t rx_bytes_{0}, rx_lines_{0};
  uint8_t pending_ok_{0};
  uint32_t pending_ms_{0};
  char line_[200]{};
  uint16_t line_len_{0};
};

}  // namespace fluidnc_pendant
}  // namespace esphome
