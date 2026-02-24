/*
 * SPDX-License-Identifier: LicenseRef-Apache-2.0
 *
 * Compile-time TX/RX GPIO pin numbers extracted from the 's21uart'
 * device-tree alias.  Works for any board whose s21uart pinctrl-0 state
 * contains NRF_PSEL(UART_TX, ...) and NRF_PSEL(UART_RX, ...) entries,
 * regardless of how the child groups are named.
 */
#pragma once

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/pinctrl/nrf-pinctrl.h>

/*
 * NRF_PSEL bit layout (zephyr/dt-bindings/pinctrl/nrf-pinctrl.h):
 *   bits [8:0]   = port*32 + pin  (== NRF_GPIO_PIN_MAP(port, pin))
 *   bits [31:24] = function code  (NRF_FUN_UART_TX / NRF_FUN_UART_RX etc.)
 */
#define _S21_PSEL_GET_PIN(psel) (static_cast<uint32_t>(psel) & 0x1FFU)
#define _S21_PSEL_GET_FUN(psel) ((static_cast<uint32_t>(psel) >> 24U) & 0xFFU)

/* Flatten every psels entry from every child group of the default pinctrl state. */
#define _S21_EMIT_PSEL(node, prop, idx) DT_PROP_BY_IDX(node, prop, idx),
#define _S21_EMIT_GROUP(node)     DT_FOREACH_PROP_ELEM(node, psels, _S21_EMIT_PSEL)

#define _S21_PINCTRL_DEFAULT DT_PHANDLE_BY_IDX(DT_ALIAS(s21uart), pinctrl_0, 0)

namespace s21_pinconfig {

static constexpr uint32_t kPsels[] = {
    DT_FOREACH_CHILD(_S21_PINCTRL_DEFAULT, _S21_EMIT_GROUP)
};

static constexpr uint32_t pinForFun(uint32_t fun)
{
    for (uint32_t psel : kPsels) {
        if (_S21_PSEL_GET_FUN(psel) == fun) {
            return _S21_PSEL_GET_PIN(psel);
        }
    }
    return UINT32_MAX;
}

/// GPIO pin number (== NRF_GPIO_PIN_MAP(port, pin)) for UART TX.
static constexpr uint32_t kTxPin = pinForFun(NRF_FUN_UART_TX);
/// GPIO pin number for UART RX.
static constexpr uint32_t kRxPin = pinForFun(NRF_FUN_UART_RX);

static_assert(kTxPin != UINT32_MAX, "UART_TX pin not found in s21uart pinctrl-0");
static_assert(kRxPin != UINT32_MAX, "UART_RX pin not found in s21uart pinctrl-0");

} // namespace s21_pinconfig

#undef _S21_EMIT_PSEL
#undef _S21_EMIT_GROUP
#undef _S21_PINCTRL_DEFAULT
#undef _S21_PSEL_GET_PIN
#undef _S21_PSEL_GET_FUN
