#!/usr/bin/env python3
"""
xccy_demo.py
------------
Standalone demonstration of the XCCY basis swap solver.
Uses hard-coded USD (dom) and EUR (for) curves approximating
current market levels (USD/EUR XCCY basis ~ -20 bps on the EUR leg,
i.e. a par receiver of EUR IBOR flat pays USD SOFR + ~(-20bps)).

This script does NOT require the compiled shared library – it implements
the same closed-form basis solve in pure Python so you can verify the
math before hooking in the C engine.

To run with the actual C engine, swap the PurePythonXCCYEngine for
XCCYBridge (from xccy_bridge.py) after building libccurve.so.
"""

import math
from typing import List

# ---------------------------------------------------------------------------
# Minimal pure-Python curve engine (mirrors getDiscountFactor log-linear)
# ---------------------------------------------------------------------------

def get_df(nodes: List[dict], t: float) -> float:
    """Log-linear interpolation of discount factors."""
    if t <= 0.0:
        return 1.0
    if t <= nodes[0]["time"]:
        return math.exp(-nodes[0]["rate"] * t)
    if t >= nodes[-1]["time"]:
        n = nodes[-1]
        return math.exp(-n["rate"] * t)
    # Find bracket
    for i in range(len(nodes) - 1):
        t0, t1 = nodes[i]["time"], nodes[i + 1]["time"]
        if t0 <= t <= t1:
            r0, r1 = nodes[i]["rate"], nodes[i + 1]["rate"]
            df0 = math.exp(-r0 * t0)
            df1 = math.exp(-r1 * t1)
            if df0 <= 0 or df1 <= 0:
                return 0.0
            log_df = math.log(df0) + (t - t0) / (t1 - t0) * (math.log(df1) - math.log(df0))
            return math.exp(log_df)
    return math.exp(-nodes[-1]["rate"] * t)


def solve_xccy_basis_bps(
    maturity:      float,
    frequency:     int,
    dom_notional:  float,
    fx_spot:       float,
    dom_fwd_nodes: List[dict],
    dom_ois_nodes: List[dict],
    for_fwd_nodes: List[dict],
    for_ois_nodes: List[dict],
) -> float:
    """
    Closed-form XCCY basis spread solve.

    Convention: domestic = USD, foreign = EUR
    basis spread goes on the domestic (USD) leg.

    Returns fair spread in basis points.
    """
    dt           = 1.0 / frequency
    n_periods    = round(maturity * frequency)
    for_notional = dom_notional / fx_spot

    pv_dom_ibor      = 0.0
    pv_for_ibor      = 0.0
    annuity_dom      = 0.0

    for i in range(1, n_periods + 1):
        t_start = (i - 1) * dt
        t_end   = i * dt
        t_pay   = t_end

        # --- Domestic (USD) leg ---
        df_dom_fwd_start = get_df(dom_fwd_nodes, t_start)
        df_dom_fwd_end   = get_df(dom_fwd_nodes, t_end)
        df_dom_ois       = get_df(dom_ois_nodes, t_pay)

        fwd_dom   = (df_dom_fwd_start / df_dom_fwd_end - 1.0) / dt
        pv_dom_ibor += dom_notional * fwd_dom * dt * df_dom_ois
        annuity_dom += dom_notional * dt * df_dom_ois

        # --- Foreign (EUR) leg ---
        df_for_fwd_start = get_df(for_fwd_nodes, t_start)
        df_for_fwd_end   = get_df(for_fwd_nodes, t_end)
        df_for_ois       = get_df(for_ois_nodes, t_pay)

        fwd_for   = (df_for_fwd_start / df_for_fwd_end - 1.0) / dt
        pv_for_ibor += for_notional * fwd_for * dt * df_for_ois

    # Notional exchanges (initial + final)
    df_dom_mat = get_df(dom_ois_nodes, maturity)
    df_for_mat = get_df(for_ois_nodes, maturity)

    pv_dom_notional = dom_notional * (df_dom_mat - 1.0)
    pv_for_notional = for_notional * (df_for_mat - 1.0) * fx_spot

    # Convert foreign leg to domestic currency
    pv_for_total = pv_for_ibor * fx_spot + pv_for_notional
    pv_dom_total = pv_dom_ibor + pv_dom_notional

    # Fair spread (linear solve)
    if abs(annuity_dom) < 1e-12:
        return 0.0

    basis_decimal = (pv_for_total - pv_dom_total) / annuity_dom
    return basis_decimal * 10_000.0


def _npv(maturity, frequency, dom_notional, fx_spot, basis_spread_bps,
         dom_fwd_nodes, dom_ois_nodes, for_fwd_nodes, for_ois_nodes) -> float:
    """Inner NPV helper (no DV01 recursion)."""
    dt           = 1.0 / frequency
    n_periods    = round(maturity * frequency)
    for_notional = dom_notional / fx_spot
    spread       = basis_spread_bps / 10_000.0
    pv_dom_coupon = pv_for_coupon = 0.0

    for i in range(1, n_periods + 1):
        t_s, t_e = (i - 1) * dt, i * dt
        df_ds = get_df(dom_fwd_nodes, t_s); df_de = get_df(dom_fwd_nodes, t_e)
        df_do = get_df(dom_ois_nodes, t_e)
        pv_dom_coupon += dom_notional * ((df_ds / df_de - 1.0) / dt + spread) * dt * df_do
        df_fs = get_df(for_fwd_nodes, t_s); df_fe = get_df(for_fwd_nodes, t_e)
        df_fo = get_df(for_ois_nodes, t_e)
        pv_for_coupon += for_notional * (df_fs / df_fe - 1.0) / dt * dt * df_fo

    pv_dom_leg = pv_dom_coupon + dom_notional * (get_df(dom_ois_nodes, maturity) - 1.0)
    pv_for_leg = pv_for_coupon * fx_spot + for_notional * (get_df(for_ois_nodes, maturity) - 1.0) * fx_spot
    return pv_for_leg - pv_dom_leg, pv_dom_leg, pv_for_leg, pv_for_coupon, for_notional


def price_xccy_swap(
    maturity:         float,
    frequency:        int,
    dom_notional:     float,
    fx_spot:          float,
    basis_spread_bps: float,
    dom_fwd_nodes:    List[dict],
    dom_ois_nodes:    List[dict],
    for_fwd_nodes:    List[dict],
    for_ois_nodes:    List[dict],
) -> dict:
    """Full NPV + greeks for a given basis spread."""
    npv, pv_dom_leg, pv_for_leg, pv_for_coupon, for_notional = _npv(
        maturity, frequency, dom_notional, fx_spot, basis_spread_bps,
        dom_fwd_nodes, dom_ois_nodes, for_fwd_nodes, for_ois_nodes)

    bp = 1e-4
    def bump(nodes, d): return [{"time": n["time"], "rate": n["rate"] + d} for n in nodes]

    npv_dom_up = _npv(maturity, frequency, dom_notional, fx_spot, basis_spread_bps,
                      bump(dom_fwd_nodes, bp), bump(dom_ois_nodes, bp),
                      for_fwd_nodes, for_ois_nodes)[0]
    npv_for_up = _npv(maturity, frequency, dom_notional, fx_spot, basis_spread_bps,
                      dom_fwd_nodes, dom_ois_nodes,
                      bump(for_fwd_nodes, bp), bump(for_ois_nodes, bp))[0]

    return {
        "npv_dom_ccy":      npv,
        "pv_dom_leg":       pv_dom_leg,
        "pv_for_leg_dom":   pv_for_leg,
        "basis_spread_bps": basis_spread_bps,
        "dv01_dom":         npv_dom_up - npv,
        "dv01_for":         npv_for_up - npv,
        "fx_delta":         0.01 * (pv_for_coupon + for_notional * get_df(for_ois_nodes, maturity)),
    }


# ---------------------------------------------------------------------------
# Sample curve data  (USD dom, EUR for – approximate 2026 levels)
# ---------------------------------------------------------------------------

# USD SOFR OIS curve (domestic)
USD_OIS = [
    {"time": 0.25,  "rate": 0.0430},
    {"time": 0.50,  "rate": 0.0420},
    {"time": 1.00,  "rate": 0.0395},
    {"time": 2.00,  "rate": 0.0375},
    {"time": 3.00,  "rate": 0.0365},
    {"time": 5.00,  "rate": 0.0380},
    {"time": 7.00,  "rate": 0.0398},
    {"time": 10.00, "rate": 0.0415},
]

# USD SOFR forward (projection) curve – slightly above OIS for term premium
USD_FWD = [
    {"time": 0.25,  "rate": 0.0432},
    {"time": 0.50,  "rate": 0.0425},
    {"time": 1.00,  "rate": 0.0400},
    {"time": 2.00,  "rate": 0.0380},
    {"time": 3.00,  "rate": 0.0372},
    {"time": 5.00,  "rate": 0.0388},
    {"time": 7.00,  "rate": 0.0406},
    {"time": 10.00, "rate": 0.0422},
]

# EUR €STR OIS curve (foreign)
EUR_OIS = [
    {"time": 0.25,  "rate": 0.0210},
    {"time": 0.50,  "rate": 0.0215},
    {"time": 1.00,  "rate": 0.0225},
    {"time": 2.00,  "rate": 0.0255},
    {"time": 3.00,  "rate": 0.0280},
    {"time": 5.00,  "rate": 0.0310},
    {"time": 7.00,  "rate": 0.0328},
    {"time": 10.00, "rate": 0.0342},
]

# EUR EURIBOR forward (projection) curve
EUR_FWD = [
    {"time": 0.25,  "rate": 0.0212},
    {"time": 0.50,  "rate": 0.0218},
    {"time": 1.00,  "rate": 0.0230},
    {"time": 2.00,  "rate": 0.0262},
    {"time": 3.00,  "rate": 0.0288},
    {"time": 5.00,  "rate": 0.0318},
    {"time": 7.00,  "rate": 0.0336},
    {"time": 10.00, "rate": 0.0350},
]

FX_SPOT      = 1.08      # 1 EUR = 1.08 USD
DOM_NOTIONAL = 10_800_000  # USD 10.8 mm  ≈  EUR 10 mm


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    tenors    = [1, 2, 3, 5, 7, 10]
    frequency = 4   # quarterly

    print("\n" + "=" * 56)
    print("  XCCY Basis Swap – Fair Spread Tenor Ladder")
    print("  DOM: USD (SOFR)   FOR: EUR (EURIBOR)   FX: {:.4f}".format(FX_SPOT))
    print("=" * 56)
    print(f"  {'Tenor':>6}  {'Fair Spread (bps)':>18}")
    print("  " + "-" * 28)

    for tenor in tenors:
        bps = solve_xccy_basis_bps(
            maturity      = tenor,
            frequency     = frequency,
            dom_notional  = DOM_NOTIONAL,
            fx_spot       = FX_SPOT,
            dom_fwd_nodes = USD_FWD,
            dom_ois_nodes = USD_OIS,
            for_fwd_nodes = EUR_FWD,
            for_ois_nodes = EUR_OIS,
        )
        print(f"  {tenor:>5}Y  {bps:>+17.2f}")

    print()

    # Full pricing for the 5-year with a traded spread of -20bps
    TRADED_BASIS_BPS = -20.0
    TENOR            = 5.0

    result = price_xccy_swap(
        maturity         = TENOR,
        frequency        = frequency,
        dom_notional     = DOM_NOTIONAL,
        fx_spot          = FX_SPOT,
        basis_spread_bps = TRADED_BASIS_BPS,
        dom_fwd_nodes    = USD_FWD,
        dom_ois_nodes    = USD_OIS,
        for_fwd_nodes    = EUR_FWD,
        for_ois_nodes    = EUR_OIS,
    )

    print("=" * 56)
    print(f"  5Y XCCY Swap Mark-to-Market  (basis = {TRADED_BASIS_BPS:+.0f} bps)")
    print("=" * 56)
    print(f"  NPV (USD)              : {result['npv_dom_ccy']:>+14,.2f}")
    print(f"  PV dom leg (USD)       : {result['pv_dom_leg']:>+14,.2f}")
    print(f"  PV for leg (USD)       : {result['pv_for_leg_dom']:>+14,.2f}")
    print(f"  DV01 dom (USD / bp)    : {result['dv01_dom']:>+14,.2f}")
    print(f"  DV01 for (USD / bp)    : {result['dv01_for']:>+14,.2f}")
    print(f"  FX delta (USD / 1% FX) : {result['fx_delta']:>+14,.2f}")
    print("=" * 56)

