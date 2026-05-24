# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make all          # Build libcurve_engine.so (C99 core)
make blob         # Compile C objects only (no shared lib)
make bloomberg    # Build libbloomberg_fetcher.so (requires BLPAPI_HOME env var)
make nzd_example  # Build NZD Bloomberg demo executable (depends on all + bloomberg)
```

Run the Streamlit dashboard:
```bash
streamlit run src/python/app.py
```

There are no automated tests or linting configurations.

## Architecture Overview

This is a quantitative finance library for interest rate curve calibration and derivatives pricing. It has three layers:

**C99 core** (`src/c/`) — compiled to `libcurve_engine.so` — does all computation. No Python dependency; the library is self-contained with `-lm`.

**Python ctypes bridge** (`src/python/curve_bridge.py`, `xccy_bridge.py`) — mirrors C structs via `ctypes.Structure` subclasses and calls into the shared library. These files are the integration boundary: any C struct field change must be reflected here.

**Streamlit UI** (`src/python/app.py`) — interactive dashboard, calls Python bridge only.

**Optional Bloomberg C++ module** (`libbloomberg_fetcher.so`) — separate shared library built from `blpapi_*.cpp`; provides live market data. Only needed for live data workflows (e.g., `examples/nzd_bloomberg_example.c`).

### C Module Responsibilities

| File | Purpose |
|------|---------|
| `dual_curve.h/c` | Dual-curve bootstrap: IBOR forward curve + OIS discount curve calibrated jointly from deposits, futures, swaps, and OIS instruments |
| `interp.h/c` | Interpolation regime dispatch — log-linear DFs, log-DF cubic spline (Hagan-West), monotone-convex, stepwise OIS (meeting schedule), parabolic/natural cubic on zero rates |
| `xccy_swap.h/c` | Cross-currency basis swap pricer: schedule builder, NPV solver, fair basis spread, DV01 via finite difference |
| `asset_swap.h/c` | Asset swap spread solver; extended bond analytics: YTM, DV01, modified duration, convexity, z-spread, upfront fee |
| `date_utils.h/c` | Year fraction conventions (Act/365, Act/360, 30/360, Act/Act ISDA, Bus/252), business day adjustment |
| `file_utils.h/c` | JSON parsing for `MarketInstrument` arrays and curve serialization |
| `blpapi_fetcher.h/cpp` | C-callable wrapper around Bloomberg C++ SDK: BDP/BDH requests, SOFR/IBOR curve instrument fetching |
| `blpapi_data_mapper.h/cpp` | Bloomberg ticker → `MarketInstrument` struct mapping, IMM futures date resolution, bulk tenor-sorted conversion |

### Key Structs (C ↔ Python boundary)

- `MarketInstrument` — deposit / future / swap / OIS instrument specification
- `InterestRateCurve` — calibrated curve: node times, zero rates, discount factors, spline coefficients
- `FloatingRateIndex` — index conventions (tenor, day count, fixing lag)
- `XCCYSwap` / `XCCYResult` — cross-currency swap trade and pricing output
- `AssetSwapSpec` — bond + fixed receiver swap

Any change to field order, type, or size in these C structs must be mirrored in the corresponding `ctypes.Structure` in `curve_bridge.py` or `xccy_bridge.py`.

### Data Flow

```
Market Data JSON / Bloomberg
        ↓
blpapi_fetcher + blpapi_data_mapper  (C++ → C interface)
        ↓  MarketInstrument[]
dual_curve: bootstrapCurve()  →  InterestRateCurve (fwd + ois)
        ├─ interp: spline setup
        └─ date_utils, file_utils
        ↓
xccy_swap / asset_swap  (consume InterestRateCurve)
        ↓
Python bridge (ctypes)  →  Streamlit app
```

### Example Market Data

JSON files in `examples/data/` show the instrument format expected by `file_utils` and the Python bridge. `market_data_meetings.json` demonstrates OIS stepwise interpolation with central bank meeting dates.
