import ctypes
import os

# Define the C Enums
class InstrumentType:
    DEPOSIT = 0
    FUTURE = 1
    SWAP = 2

# Mirror the C MarketInstrument struct
class MarketInstrument(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("startTime", ctypes.c_double),
        ("maturity", ctypes.c_double),
        ("rate", ctypes.c_double),
        ("price", ctypes.c_double),
        ("paymentFrequency", ctypes.c_int)
    ]

# Mirror the C InterestRateCurve struct (up to the fixed-size arrays we need to read)
# We map the primary data arrays: times, rates, dfs
MAX_NODES = 50
class InterestRateCurve(ctypes.Structure):
    _fields_ = [
        ("numNodes", ctypes.c_int),
        ("times", ctypes.c_double * MAX_NODES),
        ("rates", ctypes.c_double * MAX_NODES),
        ("dfs", ctypes.c_double * MAX_NODES),
        # Rest of structural memory padding to match full C-struct footprint size safely
        ("_padding", ctypes.c_byte * 4000) 
    ]

def calibrate_curve(market_instruments_list, ois_nodes_list):
    """
    Takes Python lists of dictionary inputs, pushes them into C memory, 
    runs the bootstrap, and returns calibrated numpy-friendly arrays.
    """
    # Load the compiled shared object library
    lib = ctypes.CDLL(os.path.abspath("./libcurve_engine.so"))
    
    # Configure C function signatures
    lib.create_instrument_pool.restype = ctypes.POINTER(MarketInstrument)
    lib.create_instrument_pool.argtypes = [ctypes.c_int]
    
    lib.run_calibration_bridge.argtypes = [
        ctypes.POINTER(InterestRateCurve),
        ctypes.POINTER(InterestRateCurve),
        ctypes.POINTER(MarketInstrument),
        ctypes.c_int
    ]
    
    lib.free_instrument_pool.argtypes = [ctypes.POINTER(MarketInstrument)]

    # 1. Allocate and Populate OIS Curve Structure
    ois_curve = InterestRateCurve()
    ois_curve.numNodes = len(ois_nodes_list)
    for i, node in enumerate(ois_nodes_list):
        ois_curve.times[i] = node['time']
        ois_curve.rates[i] = node['rate']
        ois_curve.dfs[i] = 1.0 / (1.0 + node['rate'] * node['time']) # Base calculation anchor

    # 2. Allocate and Populate Market Projection Instruments Pool
    n_inst = len(market_instruments_list)
    c_inst_ptr = lib.create_instrument_pool(n_inst)
    
    for i, inst in enumerate(market_instruments_list):
        c_inst_ptr[i].type = inst['type']
        c_inst_ptr[i].startTime = inst['startTime']
        c_inst_ptr[i].maturity = inst['maturity']
        c_inst_ptr[i].rate = inst.get('rate', 0.0)
        c_inst_ptr[i].price = inst.get('price', 0.0)
        c_inst_ptr[i].paymentFrequency = inst['paymentFrequency']

    # 3. Instantiate Outbound Container and Trigger Core Calibration Matrix
    fwd_curve = InterestRateCurve()
    lib.run_calibration_bridge(ctypes.byref(fwd_curve), ctypes.byref(ois_curve), c_inst_ptr, n_inst)
    
    # Extract results out of the C memory buffer before freeing pointers
    maturities = [fwd_curve.times[i] for i in range(fwd_curve.numNodes)]
    zero_rates = [fwd_curve.rates[i] for i in range(fwd_curve.numNodes)]
    
    lib.free_instrument_pool(c_inst_ptr)
    
    return maturities, zero_rates
