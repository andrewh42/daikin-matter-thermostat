/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Air conditioner shell commands — debug interface to the bridge's
 * canonical state (sync::LogicalACState) and related machinery.
 *
 * Usage:
 *   airconditioner status   # one field per line. TwinFields show
 *                           # observed / desired / inFlight / lastSrc /
 *                           # attribution; SensorFields show observed only.
 *
 * Synchronous: handlers read sync::SyncCoordinator under its internal mutex,
 * copy the state out, and format on the shell thread. Unlike s21_shell.cpp
 * there is no async callback indirection because no I/O is involved.
 */

#include "sync/logical_ac_state.h"
#include "sync/sensor_field.h"
#include "sync/sync_coordinator.h"
#include "sync/twin_field.h"

#include <app-common/zap-generated/attributes/Accessors.h>

#include <zephyr/shell/shell.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using sync::FanLevel;
using sync::FanSpeed;
using sync::LogicalACState;
using sync::ObservationSource;
using sync::OperationalMode;
using sync::RunningMode;
using sync::SensorField;
using sync::TwinField;

/* ── Value formatters ─────────────────────────────────────────────── */

void fmtBool(char* buf, size_t n, bool v)
{
    snprintf(buf, n, "%s", v ? "true" : "false");
}

void fmtOpenClosedOpt(char* buf, size_t n, std::optional<bool> v)
{
    if (v) snprintf(buf, n, "%s", *v ? "open" : "closed");
    else   snprintf(buf, n, "n/a");
}

void fmtOnOff(char* buf, size_t n, bool v)
{
    snprintf(buf, n, "%s", v ? "on" : "off");
}

void fmtTemperature(char* buf, size_t n, int16_t v)
{
    int sign = v < 0 ? -1 : 1;
    int whole = abs(v / 100);
    int frac  = abs(v % 100);
    snprintf(buf, n, "%s%d.%02dC", sign < 0 ? "-" : "", whole, frac);
}

void fmtTemperatureOpt(char* buf, size_t n, std::optional<int16_t> v)
{
    if (v) fmtTemperature(buf, n, *v);
    else   snprintf(buf, n, "n/a");
}

void fmtHumidity(char* buf, size_t n, uint8_t v)
{
    snprintf(buf, n, "%u%%", static_cast<unsigned>(v));
}

void fmtHumidityOpt(char* buf, size_t n, std::optional<uint8_t> v)
{
    if (v) fmtHumidity(buf, n, *v);
    else   snprintf(buf, n, "n/a");
}

const char* operationalModeStr(OperationalMode m)
{
    switch (m) {
    case OperationalMode::Auto:    return "Auto";
    case OperationalMode::Cool:    return "Cool";
    case OperationalMode::Heat:    return "Heat";
    case OperationalMode::FanOnly: return "FanOnly";
    case OperationalMode::Dry:     return "Dry";
    }
    return "Unknown";
}

void fmtOperationalMode(char* buf, size_t n, OperationalMode v)
{
    snprintf(buf, n, "%s", operationalModeStr(v));
}

const char* runningModeStr(RunningMode m)
{
    switch (m) {
    case RunningMode::Off:     return "Off";
    case RunningMode::Cooling: return "Cooling";
    case RunningMode::Heating: return "Heating";
    }
    return "Unknown";
}

void fmtRunningMode(char* buf, size_t n, RunningMode v)
{
    snprintf(buf, n, "%s", runningModeStr(v));
}

const char* fanLevelStr(FanLevel f)
{
    switch (f) {
    case FanLevel::Quiet:   return "Quiet";
    case FanLevel::Low:     return "Low";
    case FanLevel::MidLow:  return "MidLow";
    case FanLevel::Medium:  return "Medium";
    case FanLevel::MidHigh: return "MidHigh";
    case FanLevel::High:    return "High";
    default:                return "Unknown";
    }
}

void fmtFanSpeed(char* buf, size_t n, FanSpeed v)
{
    snprintf(buf, n, "%s", v ? fanLevelStr(*v) : "Auto");
}

const char* sourceStr(ObservationSource s)
{
    switch (s) {
    case ObservationSource::Boot:   return "Boot";
    case ObservationSource::Device: return "Device";
    case ObservationSource::Matter: return "Matter";
    default:                        return "?";
    }
}

/* ── Per-field printers ───────────────────────────────────────────── */

template <typename T, typename Fmt>
void printTwin(const struct shell* sh, const char* name,
               const TwinField<T>& t, Fmt fmt)
{
    char obs[16], des[16], inf[16];
    fmt(obs, sizeof obs, t.observed());
    fmt(des, sizeof des, t.desired());
    if (t.inFlight().has_value()) {
        fmt(inf, sizeof inf, *t.inFlight());
    } else {
        inf[0] = '-';
        inf[1] = '\0';
    }

    shell_print(sh,
        "  %-18s observed=%-10s desired=%-10s inFlight=%-10s "
        "lastSrc=%-6s attr=%s",
        name, obs, des, inf,
        sourceStr(t.lastObservedSource()),
        sourceStr(t.attribution()));
}

template <typename T, typename Fmt>
void printSensor(const struct shell* sh, const char* name,
                 const SensorField<T>& s, Fmt fmt)
{
    char obs[16];
    fmt(obs, sizeof obs, s.observed());
    shell_print(sh, "  %-18s observed=%s", name, obs);
}

/* ── Command handlers ─────────────────────────────────────────────── */

int CmdStatus(const struct shell* sh, size_t /*argc*/, char** /*argv*/)
{
    const LogicalACState s = sync::SyncCoordinator::Instance().Snapshot();

    shell_print(sh, "LogicalACState:");
    printTwin(sh, "onOff",        s.onOff,        fmtOnOff);
    printTwin(sh, "mode",         s.mode,         fmtOperationalMode);
    printTwin(sh, "heatSetpoint", s.heatSetpoint, fmtTemperature);
    printTwin(sh, "coolSetpoint", s.coolSetpoint, fmtTemperature);
    printTwin(sh, "autoSetpoint", s.autoSetpoint, fmtTemperature);
    printTwin(sh, "fan",          s.fan,          fmtFanSpeed);
    printSensor(sh, "runningMode",      s.runningMode,          fmtRunningMode);
    printSensor(sh, "indoorTemp",       s.indoorTemp,           fmtTemperatureOpt);
    printSensor(sh, "outdoorTemp",      s.outdoorTemp,          fmtTemperatureOpt);
    printSensor(sh, "humidity",         s.humidity,             fmtHumidityOpt);
    printSensor(sh, "reachable",        s.reachable,            fmtBool);
    printSensor(sh, "refrigerantValve", s.refrigerantValveOpen, fmtOpenClosedOpt);
    return 0;
}

} // namespace

/* ── Shell registration ───────────────────────────────────────────── */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_airconditioner,
    SHELL_CMD_ARG(status, NULL,
        "Print bridge LogicalACState, one field per line.\n"
        "TwinFields show observed / desired / inFlight values plus the\n"
        "last-observation source and the current attribution; SensorFields\n"
        "(sensor-only readings) show observed only.\n"
        "Usage: airconditioner status\n",
        CmdStatus, 1, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(airconditioner, &sub_airconditioner,
    "Air conditioner bridge debug commands", NULL);
