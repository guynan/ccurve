import ctypes
import os

_repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def _load_lib():
    return ctypes.CDLL(os.path.join(_repo_root, "libcurve_engine.so"))

# ── Enums (mirror C enums) ──────────────────────────────────────────────────

class InstrumentType:
    DEPOSIT    = 0
    FUTURE     = 1
    SWAP       = 2
    OIS_SWAP   = 3
    ASSET_SWAP = 4
    # FX swap: at the front of a foreign curve.
    #   'rate'  = FX spot (dom per for)
    #   'price' = FX forward outright at 'maturity'
    # Uses the passed OIS/discount curve as the domestic reference:
    #   DF_for(T) = DF_dom(T) * spot / forward
    FX_SWAP    = 5

# ── C struct mirrors ────────────────────────────────────────────────────────
# Layout must match dual_curve.h exactly (verified via sizeof() in C):
#   FloatingRateIndex = 72,  DepositSpec = 48,  FutureSpec  = 48
#   SwapSpec = 128,          FxSwapSpec  = 48,  InstrumentSpec (union) = 128
#   MarketInstrument = 152,  InterestRateCurve = 3752

class FloatingRateIndex(ctypes.Structure):
    _fields_ = [
        ("indexType",       ctypes.c_int),
        ("_pad0",           ctypes.c_int),
        ("tenorYears",      ctypes.c_double),
        ("resetLagDays",    ctypes.c_int),
        ("paymentLagDays",  ctypes.c_int),
        ("lookbackDays",    ctypes.c_int),
        ("lockoutDays",     ctypes.c_int),
        ("dcf",             ctypes.c_int),
        ("bda",             ctypes.c_int),
        ("calendarName",    ctypes.c_char * 32),
    ]

class DepositSpec(ctypes.Structure):
    _fields_ = [
        ("rate",         ctypes.c_double),
        ("dcf",          ctypes.c_int),
        ("bda",          ctypes.c_int),
        ("calendarName", ctypes.c_char * 32),
    ]

class FutureSpec(ctypes.Structure):
    _fields_ = [
        ("price",        ctypes.c_double),
        ("dcf",          ctypes.c_int),
        ("_pad",         ctypes.c_int),
        ("calendarName", ctypes.c_char * 32),
    ]

class SwapSpec(ctypes.Structure):
    _fields_ = [
        ("rate",             ctypes.c_double),
        ("paymentFrequency", ctypes.c_int),
        ("fixedDcf",         ctypes.c_int),
        ("floatDcf",         ctypes.c_int),
        ("bda",              ctypes.c_int),
        ("calendarName",     ctypes.c_char * 32),
        ("floatIndex",       FloatingRateIndex),
    ]

class FxSwapSpec(ctypes.Structure):
    _fields_ = [
        ("fxSpot",       ctypes.c_double),
        ("fxForward",    ctypes.c_double),
        ("calendarName", ctypes.c_char * 32),
    ]

class InstrumentSpec(ctypes.Union):
    _fields_ = [
        ("deposit", DepositSpec),
        ("future",  FutureSpec),
        ("swap",    SwapSpec),      # also carries OIS_SWAP
        ("fxSwap",  FxSwapSpec),
    ]

class MarketInstrument(ctypes.Structure):
    _fields_ = [
        ("type",      ctypes.c_int),
        ("_pad0",     ctypes.c_int),
        ("startTime", ctypes.c_double),
        ("maturity",  ctypes.c_double),
        ("spec",      InstrumentSpec),
    ]

MAX_NODES = 50

class InterestRateCurve(ctypes.Structure):
    _fields_ = [
        ("numNodes", ctypes.c_int),
        ("_pad0",    ctypes.c_int),
        ("times",    ctypes.c_double * MAX_NODES),
        ("rates",    ctypes.c_double * MAX_NODES),
        ("dfs",      ctypes.c_double * MAX_NODES),
        ("_rest",    ctypes.c_byte * 2600),
    ]

# Sanity-check struct sizes at import time
assert ctypes.sizeof(FloatingRateIndex)  == 72,  f"FloatingRateIndex size mismatch: {ctypes.sizeof(FloatingRateIndex)}"
assert ctypes.sizeof(DepositSpec)        == 48,  f"DepositSpec size mismatch: {ctypes.sizeof(DepositSpec)}"
assert ctypes.sizeof(FutureSpec)         == 48,  f"FutureSpec size mismatch: {ctypes.sizeof(FutureSpec)}"
assert ctypes.sizeof(SwapSpec)           == 128, f"SwapSpec size mismatch: {ctypes.sizeof(SwapSpec)}"
assert ctypes.sizeof(FxSwapSpec)         == 48,  f"FxSwapSpec size mismatch: {ctypes.sizeof(FxSwapSpec)}"
assert ctypes.sizeof(InstrumentSpec)     == 128, f"InstrumentSpec size mismatch: {ctypes.sizeof(InstrumentSpec)}"
assert ctypes.sizeof(MarketInstrument)   == 152, f"MarketInstrument size mismatch: {ctypes.sizeof(MarketInstrument)}"
assert ctypes.sizeof(InterestRateCurve) >= 3752, f"InterestRateCurve too small: {ctypes.sizeof(InterestRateCurve)}"

# ── Public API ──────────────────────────────────────────────────────────────

def calibrate_curve(market_instruments_list, ois_nodes_list):
    """
    Bootstrap a dual-curve IBOR forward curve.

    Parameters
    ----------
    market_instruments_list : list of dict
        Each dict must have 'type', 'startTime', 'maturity', 'paymentFrequency',
        and either 'rate' (DEPOSIT/SWAP) or 'price' (FUTURE).
    ois_nodes_list : list of dict
        Each dict has 'time' and 'rate' keys for the OIS discount curve nodes.

    Returns
    -------
    (maturities, zero_rates) : tuple of lists
        Year fractions and continuously-compounded zero rates at each curve node.
    """
    lib = _load_lib()

    lib.create_instrument_pool.restype  = ctypes.POINTER(MarketInstrument)
    lib.create_instrument_pool.argtypes = [ctypes.c_int]
    lib.run_calibration_bridge.argtypes = [
        ctypes.POINTER(InterestRateCurve),
        ctypes.POINTER(InterestRateCurve),
        ctypes.POINTER(MarketInstrument),
        ctypes.c_int,
    ]
    lib.free_instrument_pool.argtypes = [ctypes.POINTER(MarketInstrument)]

    ois_curve = InterestRateCurve()
    ois_curve.numNodes = len(ois_nodes_list)
    for i, node in enumerate(ois_nodes_list):
        ois_curve.times[i] = node["time"]
        ois_curve.rates[i] = node["rate"]
        ois_curve.dfs[i]   = 1.0 / (1.0 + node["rate"] * node["time"])

    n_inst    = len(market_instruments_list)
    c_inst_ptr = lib.create_instrument_pool(n_inst)
    for i, inst in enumerate(market_instruments_list):
        t = inst["type"]
        c_inst_ptr[i].type      = t
        c_inst_ptr[i].startTime = inst.get("startTime", 0.0)
        c_inst_ptr[i].maturity  = inst["maturity"]

        # Write into the correct union variant based on type.
        rate = inst.get("rate", 0.0)
        if t == InstrumentType.DEPOSIT:
            c_inst_ptr[i].spec.deposit.rate = rate
        elif t == InstrumentType.FUTURE:
            c_inst_ptr[i].spec.future.price = inst.get("price", 0.0)
        elif t in (InstrumentType.SWAP, InstrumentType.OIS_SWAP):
            c_inst_ptr[i].spec.swap.rate             = rate
            c_inst_ptr[i].spec.swap.paymentFrequency = inst.get("paymentFrequency", 2)
        elif t == InstrumentType.FX_SWAP:
            # Accept semantic names, fall back to rate/price for back-compat.
            c_inst_ptr[i].spec.fxSwap.fxSpot    = inst.get("fxSpot",    rate)
            c_inst_ptr[i].spec.fxSwap.fxForward = inst.get("fxForward", inst.get("price", 0.0))
        else:
            raise ValueError(f"Unsupported instrument type {t} at index {i}")

    fwd_curve = InterestRateCurve()
    lib.run_calibration_bridge(
        ctypes.byref(fwd_curve), ctypes.byref(ois_curve), c_inst_ptr, n_inst
    )

    maturities = [fwd_curve.times[i] for i in range(fwd_curve.numNodes)]
    zero_rates = [fwd_curve.rates[i] for i in range(fwd_curve.numNodes)]

    lib.free_instrument_pool(c_inst_ptr)
    return maturities, zero_rates
