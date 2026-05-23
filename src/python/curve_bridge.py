import ctypes
import math
import os

MAX_NODES = 50


class InstrumentType:
    DEPOSIT  = 0
    FUTURE   = 1
    SWAP     = 2
    OIS_SWAP = 3


class MarketInstrument(ctypes.Structure):
    _fields_ = [
        ("type",             ctypes.c_int),
        ("startTime",        ctypes.c_double),
        ("maturity",         ctypes.c_double),
        ("rate",             ctypes.c_double),
        ("price",            ctypes.c_double),
        ("paymentFrequency", ctypes.c_int),
        ("fixedDcf",         ctypes.c_int),
        ("floatDcf",         ctypes.c_int),
        ("bda",              ctypes.c_int),
        ("calendarName",     ctypes.c_char * 32),
    ]


class BasisCurve(ctypes.Structure):
    _fields_ = [
        ("numNodes",   ctypes.c_int),
        ("times",      ctypes.c_double * MAX_NODES),
        ("spreads",    ctypes.c_double * MAX_NODES),
        ("spreadsBps", ctypes.c_double * MAX_NODES),
    ]


class InterestRateCurve(ctypes.Structure):
    """
    Mirrors the C InterestRateCurve layout.
    numNodes/times/rates/dfs sit at the expected offsets; _tail covers the
    remaining fields (cbSchedule, regimes, numRegimes, spline arrays) so
    the C engine can write into them safely without buffer overflow.
    4096 bytes of tail is comfortably larger than the ~2544 bytes needed.
    """
    _fields_ = [
        ("numNodes", ctypes.c_int),
        ("times",    ctypes.c_double * MAX_NODES),
        ("rates",    ctypes.c_double * MAX_NODES),
        ("dfs",      ctypes.c_double * MAX_NODES),
        ("_tail",    ctypes.c_byte * 4096),
    ]


def _load_lib():
    lib = ctypes.CDLL(os.path.abspath("./libcurve_engine.so"))

    lib.create_instrument_pool.restype  = ctypes.POINTER(MarketInstrument)
    lib.create_instrument_pool.argtypes = [ctypes.c_int]

    lib.free_instrument_pool.argtypes = [ctypes.POINTER(MarketInstrument)]

    lib.run_calibration_bridge.restype  = None
    lib.run_calibration_bridge.argtypes = [
        ctypes.POINTER(InterestRateCurve),
        ctypes.POINTER(InterestRateCurve),
        ctypes.POINTER(MarketInstrument),
        ctypes.c_int,
    ]

    lib.computeBasisCurve.restype  = None
    lib.computeBasisCurve.argtypes = [
        ctypes.POINTER(InterestRateCurve),
        ctypes.POINTER(InterestRateCurve),
        ctypes.POINTER(BasisCurve),
    ]

    lib.computeParallelDV01.restype  = ctypes.c_double
    lib.computeParallelDV01.argtypes = [
        ctypes.POINTER(MarketInstrument),
        ctypes.c_int,
        ctypes.POINTER(InterestRateCurve),
        ctypes.c_double,
        ctypes.c_int,
    ]

    lib.computeKeyRateDV01.restype  = None
    lib.computeKeyRateDV01.argtypes = [
        ctypes.POINTER(MarketInstrument),
        ctypes.c_int,
        ctypes.POINTER(InterestRateCurve),
        ctypes.c_double,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_double),
    ]

    return lib


def calibrate_curve(market_instruments_list, ois_nodes_list,
                    dv01_maturity=5.0, dv01_frequency=2):
    """
    Calibrate the forward curve and compute curve analytics.

    Parameters
    ----------
    market_instruments_list : list[dict]
        Each dict has keys: type, startTime, maturity, rate (or price),
        paymentFrequency.  Optional: fixedDcf, floatDcf, bda, calendarName.
    ois_nodes_list : list[dict]
        OIS curve nodes: [{"time": float, "rate": float}, ...]
    dv01_maturity : float
        Tenor of the par swap used for key-rate DV01 (default 5Y).
    dv01_frequency : int
        Payment frequency for the DV01 target swap (default 2 = semi-annual).

    Returns
    -------
    dict with keys:
        times, rates, dfs           – forward curve nodes
        ois_times, ois_rates        – OIS curve nodes (as loaded, after anchoring)
        basis_times, basis_spreads_bps – OIS-IBOR basis in bps
        key_rate_dv01               – sensitivity per instrument (raw rate change, per 1bp bump)
        parallel_dv01               – parallel sensitivity
    """
    lib = _load_lib()

    # Build OIS curve struct
    ois_curve = InterestRateCurve()
    ois_curve.numNodes = len(ois_nodes_list)
    for i, node in enumerate(ois_nodes_list):
        t = node["time"]
        r = node["rate"]
        ois_curve.times[i] = t
        ois_curve.rates[i] = r
        ois_curve.dfs[i]   = math.exp(-r * t) if t > 0.0 else 1.0

    # Allocate and populate instrument array
    n_inst     = len(market_instruments_list)
    c_inst_ptr = lib.create_instrument_pool(n_inst)
    for i, inst in enumerate(market_instruments_list):
        c_inst_ptr[i].type             = inst["type"]
        c_inst_ptr[i].startTime        = inst["startTime"]
        c_inst_ptr[i].maturity         = inst["maturity"]
        c_inst_ptr[i].rate             = inst.get("rate",  0.0)
        c_inst_ptr[i].price            = inst.get("price", 0.0)
        c_inst_ptr[i].paymentFrequency = inst["paymentFrequency"]
        c_inst_ptr[i].fixedDcf         = inst.get("fixedDcf", 0)
        c_inst_ptr[i].floatDcf         = inst.get("floatDcf", 0)
        c_inst_ptr[i].bda              = inst.get("bda", 0)
        cal = inst.get("calendarName", "")
        c_inst_ptr[i].calendarName     = cal.encode("ascii") if cal else b""

    # Bootstrap forward curve
    fwd_curve = InterestRateCurve()
    lib.run_calibration_bridge(
        ctypes.byref(fwd_curve), ctypes.byref(ois_curve), c_inst_ptr, n_inst
    )

    # Basis curve: IBOR fwd zero − OIS zero
    basis = BasisCurve()
    lib.computeBasisCurve(
        ctypes.byref(fwd_curve), ctypes.byref(ois_curve), ctypes.byref(basis)
    )

    # Key-rate DV01
    dv01_arr = (ctypes.c_double * n_inst)()
    lib.computeKeyRateDV01(
        c_inst_ptr, n_inst, ctypes.byref(ois_curve),
        dv01_maturity, dv01_frequency, dv01_arr
    )

    # Parallel DV01
    par_dv01 = lib.computeParallelDV01(
        c_inst_ptr, n_inst, ctypes.byref(ois_curve),
        dv01_maturity, dv01_frequency
    )

    n   = fwd_curve.numNodes
    nb  = basis.numNodes
    result = {
        "times":               [fwd_curve.times[i] for i in range(n)],
        "rates":               [fwd_curve.rates[i] for i in range(n)],
        "dfs":                 [fwd_curve.dfs[i]   for i in range(n)],
        "ois_times":           [ois_curve.times[i] for i in range(ois_curve.numNodes)],
        "ois_rates":           [ois_curve.rates[i] for i in range(ois_curve.numNodes)],
        "basis_times":         [basis.times[i]      for i in range(nb)],
        "basis_spreads_bps":   [basis.spreadsBps[i] for i in range(nb)],
        "key_rate_dv01":       [dv01_arr[i]         for i in range(n_inst)],
        "parallel_dv01":       par_dv01,
    }

    lib.free_instrument_pool(c_inst_ptr)
    return result
