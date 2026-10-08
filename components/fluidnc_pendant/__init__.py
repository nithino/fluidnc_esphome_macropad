"""fluidnc_pendant - ESPHome component: a button pad that talks to FluidNC over UART.

Each button has an optional command to send when it is pressed and another when it is released.
See ../../k40-pendant.yaml for a complete example.

Command syntax (all strings are validated at compile time):
  "G0 X10"                  one G-code line (a newline is added for you)
  "M3 S100|G4 P0.4|M5"      several lines in one burst, separated by |
  "jog X-9 F3000"           shortcut for the jog line  $J=G91 G21 X-9 F3000
  HOME UNLOCK               shortcuts for  $H  and  $X
  "0x85" or "0x85 0x18"     raw realtime byte(s), no newline
  STATUS HOLD RESUME CYCLE_START JOG_CANCEL RESET SAFETY_DOOR
                            names for the FluidNC realtime bytes  ? ! ~ ~ 0x85 0x18 0x84
Why the shortcuts: ESPHome reads $H / $J as substitution variables and prints a warning
(a literal $H still works, it is only noisy, and $$ does NOT escape it in this field).
"""
import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart
from esphome.const import CONF_ID, CONF_NAME, CONF_PIN

DEPENDENCIES = ["uart"]
CODEOWNERS = []

fluidnc_pendant_ns = cg.esphome_ns.namespace("fluidnc_pendant")
FluidNCPendant = fluidnc_pendant_ns.class_("FluidNCPendant", cg.Component, uart.UARTDevice)

CONF_BUTTONS = "buttons"
CONF_ON_PRESS = "on_press"
CONF_ON_PRESS_ALT = "on_press_alt"
CONF_ALT_STATES = "alt_states"
CONF_ON_RELEASE = "on_release"
CONF_REQUIRE_STATE = "require_state"
CONF_HOLD_TIME = "hold_time"
CONF_REPEAT_INTERVAL = "repeat_interval"
CONF_LOCKOUT = "lockout"
CONF_DRY_RUN = "dry_run"
CONF_LOG_RX = "log_rx"
CONF_STATUS_POLL = "status_poll_interval"
CONF_STATUS_TIMEOUT = "status_timeout"
CONF_DEBOUNCE = "debounce"

# Order MUST match enum MachState in fluidnc_pendant.h (UNKNOWN = 0, so these start at 1)
STATES = ["idle", "run", "hold", "jog", "alarm", "door", "check", "home", "sleep"]

ALIASES = {
    "STATUS": b"?",
    "HOLD": b"!",
    "RESUME": b"~",
    "CYCLE_START": b"~",
    "JOG_CANCEL": b"\x85",
    "RESET": b"\x18",
    "SAFETY_DOOR": b"\x84",
    "HOME": b"$H\n",
    "UNLOCK": b"$X\n",
}


def encode_command(value) -> bytes:
    value = cv.string_strict(value).strip()
    if not value:
        raise cv.Invalid("Command must not be empty")
    upper = value.upper()
    if upper in ALIASES:
        return ALIASES[upper]
    if upper.startswith("0X"):
        out = bytearray()
        for token in value.split():
            if not re.fullmatch(r"0[xX][0-9A-Fa-f]{2}", token):
                raise cv.Invalid(f"Bad raw byte '{token}', expected like 0x85")
            out.append(int(token, 16))
        return bytes(out)
    try:
        value.encode("ascii")
    except UnicodeEncodeError as err:
        raise cv.Invalid("G-code lines must be plain ASCII") from err
    lines = [part.strip() for part in value.split("|")]
    if any(not line for line in lines):
        raise cv.Invalid("Empty G-code line (check for a stray | )")
    lines = [
        "$J=G91 G21 " + line[4:].strip() if re.match(r"(?i)^jog\s", line) else line
        for line in lines
    ]
    return ("\n".join(lines) + "\n").encode("ascii")


def command(value):
    encode_command(value)  # validate only; the original text is kept for readable logs/config dumps
    return value


def state_mask(names) -> int:
    mask = 0
    for name in names or []:
        mask |= 1 << (STATES.index(name) + 1)
    return mask


state_list = cv.ensure_list(cv.one_of(*STATES, lower=True))


def _button_checks(config):
    if CONF_ON_PRESS not in config and CONF_ON_RELEASE not in config:
        raise cv.Invalid("A button needs at least on_press or on_release")
    if (CONF_ON_PRESS_ALT in config) != (CONF_ALT_STATES in config):
        raise cv.Invalid("on_press_alt and alt_states must be used together")
    if CONF_ON_PRESS_ALT in config and CONF_ON_PRESS not in config:
        raise cv.Invalid("on_press_alt needs on_press as well")
    if CONF_REPEAT_INTERVAL in config and config[CONF_REPEAT_INTERVAL].total_milliseconds > 0 and CONF_ON_PRESS not in config:
        raise cv.Invalid("repeat_interval needs on_press")
    return config


BUTTON_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_NAME): cv.string,
            cv.Required(CONF_PIN): pins.internal_gpio_input_pullup_pin_schema,
            cv.Optional(CONF_ON_PRESS): command,
            cv.Optional(CONF_ON_PRESS_ALT): command,
            cv.Optional(CONF_ALT_STATES): state_list,
            cv.Optional(CONF_ON_RELEASE): command,
            cv.Optional(CONF_REQUIRE_STATE): state_list,
            cv.Optional(CONF_HOLD_TIME, default="0ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_REPEAT_INTERVAL, default="0ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LOCKOUT, default="0ms"): cv.positive_time_period_milliseconds,
        }
    ),
    _button_checks,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FluidNCPendant),
            cv.Required(CONF_BUTTONS): cv.All(cv.ensure_list(BUTTON_SCHEMA), cv.Length(min=1, max=16)),
            cv.Optional(CONF_DRY_RUN, default=False): cv.boolean,
            cv.Optional(CONF_LOG_RX, default=False): cv.boolean,
            cv.Optional(CONF_STATUS_POLL, default="250ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_STATUS_TIMEOUT, default="1s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_DEBOUNCE, default="25ms"): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "fluidnc_pendant", require_tx=True, require_rx=True
)


def _hex(value) -> str:
    return encode_command(value).hex().upper() if value is not None else ""


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_dry_run(config[CONF_DRY_RUN]))
    cg.add(var.set_log_rx(config[CONF_LOG_RX]))
    cg.add(var.set_status_poll_ms(config[CONF_STATUS_POLL].total_milliseconds))
    cg.add(var.set_status_timeout_ms(config[CONF_STATUS_TIMEOUT].total_milliseconds))
    cg.add(var.set_debounce_ms(config[CONF_DEBOUNCE].total_milliseconds))

    for button in config[CONF_BUTTONS]:
        pin = await cg.gpio_pin_expression(button[CONF_PIN])
        cg.add(
            var.add_button(
                pin,
                button[CONF_NAME],
                _hex(button.get(CONF_ON_PRESS)),
                _hex(button.get(CONF_ON_RELEASE)),
                _hex(button.get(CONF_ON_PRESS_ALT)),
                state_mask(button.get(CONF_ALT_STATES)),
                state_mask(button.get(CONF_REQUIRE_STATE)),
                button[CONF_HOLD_TIME].total_milliseconds,
                button[CONF_REPEAT_INTERVAL].total_milliseconds,
                button[CONF_LOCKOUT].total_milliseconds,
            )
        )
