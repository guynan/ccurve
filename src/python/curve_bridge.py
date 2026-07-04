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
# These must exactly match the C structs in dual_curve.h.
# Verified sizes (from sizeof() output):
#   FloatingRateIndex  = 72 bytes
#   MarketInstrument   = 160 bytes
#   InterestRateCurve  = 3752 bytes

class FloatingRateIndex(ctypes.Structure):
    _fields_ = [
        ("indexType",       ctypes.c_int),
        ("_pad0",           ctypes.c_int),      # alignment before double
        ("tenorYears",      ctypes.c_double),
        ("resetLagDays",    ctypes.c_int),
        ("paymentLagDays",  ctypes.c_int),
        ("lookbackDays",    ctypes.c_int),
        ("lockoutDays",     ctypes.c_int),
        ("dcf",             ctypes.c_int),
        ("bda",             ctypes.c_int),
        ("calendarName",    ctypes.c_char * 32),
    ]

class MarketInstrument(ctypes.Structure):
    _fields_ = [
        ("type",            ctypes.c_int),
        ("_pad0",           ctypes.c_int),      # alignment before double
        ("startTime",       ctypes.c_double),
        ("maturity",        ctypes.c_double),
        ("rate",            ctypes.c_double),
        ("price",           ctypes.c_double),
        ("paymentFrequency", ctypes.c_int),
        ("fixedDcf",        ctypes.c_int),
        ("floatDcf",        ctypes.c_int),
        ("bda",             ctypes.c_int),
        ("calendarName",    ctypes.c_char * 32),
        ("floatIndex",      FloatingRateIndex),
    ]

MAX_NODES = 50

class InterestRateCurve(ctypes.Structure):
    # Expose the first four fields; the rest are covered by _padding.
    # C struct total = 3752 bytes; our Python struct must be >= that.
    _fields_ = [
        ("numNodes", ctypes.c_int),
        ("_pad0",    ctypes.c_int),             # alignment before doubles
        ("times",    ctypes.c_double * MAX_NODES),
        ("rates",    ctypes.c_double * MAX_NODES),
        ("dfs",      ctypes.c_double * MAX_NODES),
        ("_rest",    ctypes.c_byte * 2600),     # cbSchedule + regimes + splines
    ]

# Sanity-check struct sizes at import time
assert ctypes.sizeof(FloatingRateIndex)  == 72,  f"FloatingRateIndex size mismatch: {ctypes.sizeof(FloatingRateIndex)}"
assert ctypes.sizeof(MarketInstrument)   == 160, f"MarketInstrument size mismatch: {ctypes.sizeof(MarketInstrument)}"
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
        c_inst_ptr[i].type             = inst["type"]
        c_inst_ptr[i].startTime        = inst.get("startTime", 0.0)
        c_inst_ptr[i].maturity         = inst["maturity"]
        c_inst_ptr[i].rate             = inst.get("rate", 0.0)
        c_inst_ptr[i].price            = inst.get("price", 0.0)
        c_inst_ptr[i].paymentFrequency = inst.get("paymentFrequency", 2)

    fwd_curve = InterestRateCurve()
    lib.run_calibration_bridge(
        ctypes.byref(fwd_curve), ctypes.byref(ois_curve), c_inst_ptr, n_inst
    )

    maturities = [fwd_curve.times[i] for i in range(fwd_curve.numNodes)]
    zero_rates = [fwd_curve.rates[i] for i in range(fwd_curve.numNodes)]

    lib.free_instrument_pool(c_inst_ptr)
    return maturities, zero_rates
