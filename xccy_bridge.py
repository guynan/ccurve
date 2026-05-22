"""
xccy_bridge.py
--------------
Python ctypes wrapper for the XCCY basis swap engine (xccy_swap.c),
extending the existing ccurve dual-curve framework in curve_bridge.py.

Usage example
-------------
    from xccy_bridge import XCCYBridge

    bridge = XCCYBridge()

    # Four curves: (dom_fwd, dom_ois, for_fwd, for_ois)
    # Each is a list of {"time": float, "rate": float} dicts
    fair_spread_bps = bridge.solve_basis_spread(
        maturity=5.0,
        frequency=4,           # quarterly
        dom_notional=10_000_000,
        fx_spot=1.08,          # USD per EUR  →  dom=USD, for=EUR
        resettable=0,
        dom_fwd_nodes=[...],
        dom_ois_nodes=[...],
        for_fwd_nodes=[...],
        for_ois_nodes=[...],
    )
    print(f"Fair XCCY basis: {fair_spread_bps:.2f} bps")

    result = bridge.price_xccy_swap(
        maturity=5.0,
        frequency=4,
        dom_notional=10_000_000,
        fx_spot=1.08,
        basis_spread_bps=-20.0,   # current quoted spread
        resettable=0,
        dom_fwd_nodes=[...],
        dom_ois_nodes=[...],
        for_fwd_nodes=[...],
        for_ois_nodes=[...],
    )
    print(result)
"""

import ctypes
import os
from dataclasses import dataclass
from typing import List

# ---------------------------------------------------------------------------
# C struct mirrors  (must match xccy_swap.h / dual_curve.h exactly)
# ---------------------------------------------------------------------------

MAX_NODES        = 50
MAX_XCCY_PERIODS = 240

# ---- SwapCashFlow (from dual_curve.h) ----
class SwapCashFlow(ctypes.Structure):
    _fields_ = [
        ("startTime",       ctypes.c_double),
        ("endTime",         ctypes.c_double),
        ("paymentTime",     ctypes.c_double),
        ("accrualFraction", ctypes.c_double),
        ("fixedRate",       ctypes.c_double),
        ("spread",          ctypes.c_double),
        ("notional",        ctypes.c_double),
    ]

# ---- XCCYLeg ----
class XCCYLeg(ctypes.Structure):
    _fields_ = [
        ("periods",    SwapCashFlow * MAX_XCCY_PERIODS),
        ("numPeriods", ctypes.c_int),
        ("isFixed",    ctypes.c_int),
        ("notional",   ctypes.c_double),
    ]

# ---- XCCYSwap ----
class XCCYSwap(ctypes.Structure):
    _fields_ = [
        ("domLeg",              XCCYLeg),
        ("forLeg",              XCCYLeg),
        ("fxSpot",              ctypes.c_double),
        ("maturity",            ctypes.c_double),
        ("resettableNotional",  ctypes.c_int),
    ]

# ---- XCCYResult ----
class XCCYResult(ctypes.Structure):
    _fields_ = [
        ("npvDomCcy",      ctypes.c_double),
        ("basisSpreadBps", ctypes.c_double),
        ("pvDomLeg",       ctypes.c_double),
        ("pvForLegDom",    ctypes.c_double),
        ("dv01Dom",        ctypes.c_double),
        ("dv01For",        ctypes.c_double),
        ("fxDelta",        ctypes.c_double),
    ]

# ---- CentralBankSchedule (needed to size InterestRateCurve correctly) ----
class CentralBankSchedule(ctypes.Structure):
    _fields_ = [
        ("meetingTimes", ctypes.c_double * MAX_NODES),
        ("targetRates",  ctypes.c_double * MAX_NODES),
        ("numMeetings",  ctypes.c_int),
    ]

# ---- InterpolationRegime ----
# The function pointer is opaque from Python; we size it as a void* pair.
class InterpolationRegime(ctypes.Structure):
    _fields_ = [
        ("upper_time_boundary", ctypes.c_double),
        ("interp_func",         ctypes.c_void_p),
    ]

# ---- InterestRateCurve ----
class InterestRateCurve(ctypes.Structure):
    _fields_ = [
        ("numNodes",  ctypes.c_int),
        ("times",     ctypes.c_double * MAX_NODES),
        ("rates",     ctypes.c_double * MAX_NODES),
        ("dfs",       ctypes.c_double * MAX_NODES),
        ("cbSchedule",CentralBankSchedule),
        ("regimes",   InterpolationRegime * 5),
        ("numRegimes",ctypes.c_int),
        ("spline_a",  ctypes.c_double * MAX_NODES),
        ("spline_b",  ctypes.c_double * MAX_NODES),
        ("spline_c",  ctypes.c_double * MAX_NODES),
        ("spline_d",  ctypes.c_double * MAX_NODES),
    ]


# ---------------------------------------------------------------------------
# Python-friendly result dataclass
# ---------------------------------------------------------------------------

@dataclass
class XCCYPricingResult:
    npv_dom_ccy:       float
    basis_spread_bps:  float
    pv_dom_leg:        float
    pv_for_leg_dom:    float
    dv01_dom:          float
    dv01_for:          float
    fx_delta:          float

    def __str__(self):
        lines = [
            "=" * 52,
            "  XCCY Basis Swap Pricing Result",
            "=" * 52,
            f"  NPV (dom ccy)          : {self.npv_dom_ccy:+14,.4f}",
            f"  PV dom leg (dom ccy)   : {self.pv_dom_leg:+14,.4f}",
            f"  PV for leg (dom ccy)   : {self.pv_for_leg_dom:+14,.4f}",
            f"  Basis spread           : {self.basis_spread_bps:+10.2f} bps",
            f"  DV01 dom curves        : {self.dv01_dom:+14,.4f}",
            f"  DV01 for curves        : {self.dv01_for:+14,.4f}",
            f"  FX delta (1% move)     : {self.fx_delta:+14,.4f}",
            "=" * 52,
        ]
        return "\n".join(lines)


# ---------------------------------------------------------------------------
# Bridge class
# ---------------------------------------------------------------------------

class XCCYBridge:
    """
    Thin Python wrapper around xccy_swap.c exported functions.

    The shared library is expected to export:
        buildXCCYSwap
        priceXCCYSwap
        solveXCCYBasisSpread
        computeXCCYDV01

    To build the shared library (from the ccurve repo root):
        gcc -O2 -shared -fPIC -o libccurve.so \
            date_utils.c dual_curve.c xccy_swap.c -lm
    """

    def __init__(self, lib_path: str = "./libccurve.so"):
        if not os.path.exists(lib_path):
            raise FileNotFoundError(
                f"Shared library not found at '{lib_path}'.\n"
                "Build it with:\n"
                "  gcc -O2 -shared -fPIC -o libccurve.so \\\n"
                "      date_utils.c dual_curve.c xccy_swap.c -lm"
            )
        self._lib = ctypes.CDLL(os.path.abspath(lib_path))
        self._bind_signatures()

    # ------------------------------------------------------------------
    # Private: bind C function signatures
    # ------------------------------------------------------------------

    def _bind_signatures(self):
        lib = self._lib

        # void buildXCCYSwap(XCCYSwap*, double, int, double, double, double, int)
        lib.buildXCCYSwap.restype  = None
        lib.buildXCCYSwap.argtypes = [
            ctypes.POINTER(XCCYSwap),
            ctypes.c_double,   # maturity
            ctypes.c_int,      # frequency
            ctypes.c_double,   # domNotional
            ctypes.c_double,   # fxSpot
            ctypes.c_double,   # basisSpreadBps
            ctypes.c_int,      # resettable
        ]

        # void priceXCCYSwap(XCCYSwap*, IRC*, IRC*, IRC*, IRC*, double, XCCYResult*)
        lib.priceXCCYSwap.restype  = None
        lib.priceXCCYSwap.argtypes = [
            ctypes.POINTER(XCCYSwap),
            ctypes.POINTER(InterestRateCurve),  # domFwdCurve
            ctypes.POINTER(InterestRateCurve),  # domOisCurve
            ctypes.POINTER(InterestRateCurve),  # forFwdCurve
            ctypes.POINTER(InterestRateCurve),  # forOisCurve
            ctypes.c_double,                    # fxSpotNow
            ctypes.POINTER(XCCYResult),
        ]

        # double solveXCCYBasisSpread(double, int, double, double, int,
        #                             IRC*, IRC*, IRC*, IRC*)
        lib.solveXCCYBasisSpread.restype  = ctypes.c_double
        lib.solveXCCYBasisSpread.argtypes = [
            ctypes.c_double,   # maturity
            ctypes.c_int,      # frequency
            ctypes.c_double,   # domNotional
            ctypes.c_double,   # fxSpot
            ctypes.c_int,      # resettable
            ctypes.POINTER(InterestRateCurve),  # domFwdCurve
            ctypes.POINTER(InterestRateCurve),  # domOisCurve
            ctypes.POINTER(InterestRateCurve),  # forFwdCurve
            ctypes.POINTER(InterestRateCurve),  # forOisCurve
        ]

        # void computeXCCYDV01(XCCYSwap*, IRC*, IRC*, IRC*, IRC*, double,
        #                       double*, double*)
        lib.computeXCCYDV01.restype  = None
        lib.computeXCCYDV01.argtypes = [
            ctypes.POINTER(XCCYSwap),
            ctypes.POINTER(InterestRateCurve),
            ctypes.POINTER(InterestRateCurve),
            ctypes.POINTER(InterestRateCurve),
            ctypes.POINTER(InterestRateCurve),
            ctypes.c_double,
            ctypes.POINTER(ctypes.c_double),
            ctypes.POINTER(ctypes.c_double),
        ]

    # ------------------------------------------------------------------
    # Private: build InterestRateCurve from list of {"time", "rate"} dicts
    # ------------------------------------------------------------------

    @staticmethod
    def _build_curve(nodes: List[dict]) -> InterestRateCurve:
        curve = InterestRateCurve()
        n = min(len(nodes), MAX_NODES)
        curve.numNodes = n
        for i, node in enumerate(nodes[:n]):
            t = float(node["time"])
            r = float(node["rate"])
            curve.times[i] = t
            curve.rates[i] = r
            curve.dfs[i]   = (1.0 / (1.0 + r * t)) if t > 0 else 1.0
        return curve

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def solve_basis_spread(
        self,
        maturity:       float,
        frequency:      int,
        dom_notional:   float,
        fx_spot:        float,
        resettable:     int,
        dom_fwd_nodes:  List[dict],
        dom_ois_nodes:  List[dict],
        for_fwd_nodes:  List[dict],
        for_ois_nodes:  List[dict],
    ) -> float:
        """
        Returns the fair XCCY basis spread in basis points.

        Parameters
        ----------
        maturity       : swap tenor in years
        frequency      : payment frequency (4 = quarterly)
        dom_notional   : notional in domestic currency
        fx_spot        : spot FX rate  (dom per for, e.g. 1.08 for EURUSD quoted as 1 EUR = 1.08 USD)
        resettable     : 0 = fixed notional, 1 = MtM-reset (currently uses same pricing,
                         extend with forward FX for full MtM)
        *_nodes        : list of {"time": float, "rate": float} dicts
        """
        dom_fwd = self._build_curve(dom_fwd_nodes)
        dom_ois = self._build_curve(dom_ois_nodes)
        for_fwd = self._build_curve(for_fwd_nodes)
        for_ois = self._build_curve(for_ois_nodes)

        bps = self._lib.solveXCCYBasisSpread(
            ctypes.c_double(maturity),
            ctypes.c_int(frequency),
            ctypes.c_double(dom_notional),
            ctypes.c_double(fx_spot),
            ctypes.c_int(resettable),
            ctypes.byref(dom_fwd),
            ctypes.byref(dom_ois),
            ctypes.byref(for_fwd),
            ctypes.byref(for_ois),
        )
        return float(bps)

    def price_xccy_swap(
        self,
        maturity:         float,
        frequency:        int,
        dom_notional:     float,
        fx_spot:          float,
        basis_spread_bps: float,
        resettable:       int,
        dom_fwd_nodes:    List[dict],
        dom_ois_nodes:    List[dict],
        for_fwd_nodes:    List[dict],
        for_ois_nodes:    List[dict],
        compute_dv01:     bool = True,
    ) -> XCCYPricingResult:
        """
        Mark-to-market a single XCCY basis swap.

        Returns an XCCYPricingResult dataclass.
        """
        dom_fwd = self._build_curve(dom_fwd_nodes)
        dom_ois = self._build_curve(dom_ois_nodes)
        for_fwd = self._build_curve(for_fwd_nodes)
        for_ois = self._build_curve(for_ois_nodes)

        swap   = XCCYSwap()
        result = XCCYResult()

        self._lib.buildXCCYSwap(
            ctypes.byref(swap),
            ctypes.c_double(maturity),
            ctypes.c_int(frequency),
            ctypes.c_double(dom_notional),
            ctypes.c_double(fx_spot),
            ctypes.c_double(basis_spread_bps),
            ctypes.c_int(resettable),
        )

        self._lib.priceXCCYSwap(
            ctypes.byref(swap),
            ctypes.byref(dom_fwd),
            ctypes.byref(dom_ois),
            ctypes.byref(for_fwd),
            ctypes.byref(for_ois),
            ctypes.c_double(fx_spot),
            ctypes.byref(result),
        )

        dv01_dom = result.dv01Dom
        dv01_for = result.dv01For

        if compute_dv01:
            c_dv01_dom = ctypes.c_double(0.0)
            c_dv01_for = ctypes.c_double(0.0)
            self._lib.computeXCCYDV01(
                ctypes.byref(swap),
                ctypes.byref(dom_fwd),
                ctypes.byref(dom_ois),
                ctypes.byref(for_fwd),
                ctypes.byref(for_ois),
                ctypes.c_double(fx_spot),
                ctypes.byref(c_dv01_dom),
                ctypes.byref(c_dv01_for),
            )
            dv01_dom = c_dv01_dom.value
            dv01_for = c_dv01_for.value

        return XCCYPricingResult(
            npv_dom_ccy      = result.npvDomCcy,
            basis_spread_bps = result.basisSpreadBps,
            pv_dom_leg       = result.pvDomLeg,
            pv_for_leg_dom   = result.pvForLegDom,
            dv01_dom         = dv01_dom,
            dv01_for         = dv01_for,
            fx_delta         = result.fxDelta,
        )

    def tenor_ladder(
        self,
        tenors:           List[float],
        frequency:        int,
        dom_notional:     float,
        fx_spot:          float,
        resettable:       int,
        dom_fwd_nodes:    List[dict],
        dom_ois_nodes:    List[dict],
        for_fwd_nodes:    List[dict],
        for_ois_nodes:    List[dict],
    ) -> List[dict]:
        """
        Solve fair basis spread across a tenor ladder.

        Returns a list of dicts with keys: tenor, basis_spread_bps
        """
        results = []
        for tenor in tenors:
            bps = self.solve_basis_spread(
                maturity      = tenor,
                frequency     = frequency,
                dom_notional  = dom_notional,
                fx_spot       = fx_spot,
                resettable    = resettable,
                dom_fwd_nodes = dom_fwd_nodes,
                dom_ois_nodes = dom_ois_nodes,
                for_fwd_nodes = for_fwd_nodes,
                for_ois_nodes = for_ois_nodes,
            )
            results.append({"tenor": tenor, "basis_spread_bps": round(bps, 4)})
        return results
