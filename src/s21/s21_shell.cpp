/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 */

/*
 * S21 shell commands — debug interface to the S21 presentation layer.
 *
 * Usage:
 *   s21 get_operation
 *   s21 set_operation <on|off> <mode> <setpoint_C> <fanmode>
 *
 * Mode strings:     auto_cool | auto | dry | cool | heat | fan | auto_heat
 * Fan mode strings: low | midlow | medium | midhigh | high | auto | quiet
 * Setpoint:         integer degrees C, range 10–32
 *
 * The S21 presentation API is asynchronous — callbacks fire in the system
 * work queue thread. The shell command stores a module-level `sh` pointer
 * before dispatching the async call, then returns 0 immediately (the shell
 * prompt reappears). The callback prints the result via shell_print(s_shell).
 *
 * This is safe because:
 *   - shell instances are statically allocated (pointer remains valid forever)
 *   - shell_print() is callable from any thread (not just ISR)
 *   - S21DataLinkUart enforces that only one operation is in-flight at a time,
 *     so s_shell cannot be overwritten before the callback fires
 */

#include "S21Presentation.h"
#include "S21Stack.h"

#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

namespace {

const struct shell *s_shell = nullptr;

/* ── Argument parsing ─────────────────────────────────────────────── */

const char *parseOnOff(const char *str, bool &out)
{
    if (strcmp(str, "on") == 0)  { out = true;  return nullptr; }
    if (strcmp(str, "off") == 0) { out = false; return nullptr; }
    return "expected 'on' or 'off'";
}

const char *parseMode(const char *str, OperatingMode &out)
{
    static const struct { const char *name; OperatingMode mode; } kTable[] = {
        { "auto_cool", OperatingMode::Auto_Cooling },
        { "auto",      OperatingMode::Auto         },
        { "dry",       OperatingMode::Dry           },
        { "cool",      OperatingMode::Cool          },
        { "heat",      OperatingMode::Heat          },
        { "fan",       OperatingMode::FanOnly       },
        { "auto_heat", OperatingMode::Auto_Heating  },
    };
    for (auto &e : kTable) {
        if (strcmp(str, e.name) == 0) { out = e.mode; return nullptr; }
    }
    return "unknown mode (auto_cool|auto|dry|cool|heat|fan|auto_heat)";
}

const char *parseSetpoint(const char *str, int16_t &out)
{
    char *end;
    long val = strtol(str, &end, 10);
    if (*end != '\0')           { return "setpoint must be an integer"; }
    if (val < 10 || val > 32)   { return "setpoint out of range (10-32 degC)"; }
    out = static_cast<int16_t>(val * 100);
    return nullptr;
}

const char *parseFanMode(const char *str, FanMode &out)
{
    static const struct { const char *name; FanMode mode; } kTable[] = {
        { "low",     FanMode::Low     },
        { "midlow",  FanMode::MidLow  },
        { "medium",  FanMode::Medium  },
        { "midhigh", FanMode::MidHigh },
        { "high",    FanMode::High    },
        { "auto",    FanMode::Auto    },
        { "quiet",   FanMode::Quiet   },
    };
    for (auto &e : kTable) {
        if (strcmp(str, e.name) == 0) { out = e.mode; return nullptr; }
    }
    return "unknown fan mode (low|midlow|medium|midhigh|high|auto|quiet)";
}

/* ── Display helpers ──────────────────────────────────────────────── */

const char *operatingModeStr(OperatingMode m)
{
    switch (m) {
    case OperatingMode::Auto_Cooling: return "auto_cool";
    case OperatingMode::Auto:         return "auto";
    case OperatingMode::Dry:          return "dry";
    case OperatingMode::Cool:         return "cool";
    case OperatingMode::Heat:         return "heat";
    case OperatingMode::FanOnly:      return "fan";
    case OperatingMode::Auto_Heating: return "auto_heat";
    default:                          return "unknown";
    }
}

const char *fanModeStr(FanMode f)
{
    switch (f) {
    case FanMode::Low:     return "low";
    case FanMode::MidLow:  return "midlow";
    case FanMode::Medium:  return "medium";
    case FanMode::MidHigh: return "midhigh";
    case FanMode::High:    return "high";
    case FanMode::Auto:    return "auto";
    case FanMode::Quiet:   return "quiet";
    default:               return "unknown";
    }
}

/* ── Command handlers ─────────────────────────────────────────────── */

static int CmdS21GetOperation(const struct shell *sh, size_t argc, char **argv)
{
    s_shell = sh;

    S21Stack::Instance().GetPresentation().getOperation(
        [](tl::expected<S21Presentation::GetOperationResult, S21PresentationError> result) {
            const struct shell *sh = s_shell;
            if (!result) {
                shell_error(sh, "s21 get_operation error: %s", result.error().what());
                return;
            }
            auto [onOff, mode, setPoint, fanMode] = *result;
            shell_print(sh, "power:    %s",           onOff ? "on" : "off");
            shell_print(sh, "mode:     %s",           operatingModeStr(mode));
            shell_print(sh, "setpoint: %d.%02d degC",
                        static_cast<int>(setPoint / 100),
                        static_cast<int>(setPoint % 100));
            shell_print(sh, "fan:      %s",           fanModeStr(fanMode));
        });

    return 0;
}

/* argv: [0]="set_operation" [1]=on|off [2]=mode [3]=setpoint_C [4]=fanmode */
static int CmdS21SetOperation(const struct shell *sh, size_t argc, char **argv)
{
    bool onOff;
    OperatingMode mode;
    int16_t setPoint;
    FanMode fanMode;
    const char *err;

    err = parseOnOff(argv[1], onOff);
    if (err) { shell_error(sh, "arg 'on|off': %s", err);   return -EINVAL; }

    err = parseMode(argv[2], mode);
    if (err) { shell_error(sh, "arg 'mode': %s", err);     return -EINVAL; }

    err = parseSetpoint(argv[3], setPoint);
    if (err) { shell_error(sh, "arg 'setpoint': %s", err); return -EINVAL; }

    err = parseFanMode(argv[4], fanMode);
    if (err) { shell_error(sh, "arg 'fanmode': %s", err);  return -EINVAL; }

    s_shell = sh;

    S21Stack::Instance().GetPresentation().setOperation(
        onOff, mode, setPoint, fanMode,
        [](tl::expected<void, S21DataLinkError> result) {
            const struct shell *sh = s_shell;
            if (!result) {
                shell_error(sh, "s21 set_operation error: %s", result.error().what());
                return;
            }
            shell_print(sh, "s21 set_operation: OK");
        });

    return 0;
}

} // namespace

/* ── Shell registration ───────────────────────────────────────────── */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_s21,
    SHELL_CMD_ARG(get_operation, NULL,
        "Query AC state via S21 (async — result printed after ACK).\n"
        "Usage: s21 get_operation\n",
        CmdS21GetOperation, 1, 0),
    SHELL_CMD_ARG(set_operation, NULL,
        "Set AC state via S21 (async — result printed after ACK).\n"
        "Usage: s21 set_operation <on|off> <mode> <setpoint_C> <fanmode>\n"
        "  mode:     auto_cool|auto|dry|cool|heat|fan|auto_heat\n"
        "  setpoint: integer degrees C (10-32)\n"
        "  fanmode:  low|midlow|medium|midhigh|high|auto|quiet\n",
        CmdS21SetOperation, 5, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(s21, &sub_s21, "S21 Daikin serial interface commands", NULL);
