import streamlit as st
import pandas as pd
import plotly.express as px
import plotly.graph_objects as go
from curve_bridge import calibrate_curve, InstrumentType

st.set_page_config(page_title="Dual-Curve OIS Calibration Dashboard", layout="wide")
st.title("Fixed Income Dual-Curve Engine")
st.caption("C99 bootstrap backend · Python ctypes bridge · Multi-regime interpolation")

# ---------------------------------------------------------------------------
# Static OIS curve nodes (loaded from JSON in production; hardcoded here for demo)
# ---------------------------------------------------------------------------
OIS_NODES = [
    {"time": 0.25, "rate": 0.0350},
    {"time": 0.50, "rate": 0.0365},
    {"time": 0.75, "rate": 0.0375},
    {"time": 1.00, "rate": 0.0385},
    {"time": 2.00, "rate": 0.0400},
    {"time": 3.00, "rate": 0.0415},
    {"time": 5.00, "rate": 0.0430},
]

DEPO_MATURITY = 0.25   # 3-month deposit

# ---------------------------------------------------------------------------
# Controls (left column)
# ---------------------------------------------------------------------------
col_ctrl, col_charts = st.columns([1, 2])

with col_ctrl:
    st.subheader("Instrument Parameters")

    depo_rate = st.slider(
        "Deposit Rate (3M)", 0.01, 0.08, 0.0400, step=0.0005, format="%.4f"
    )

    num_futures = st.slider("Number of Futures", 1, 8, 3)

    futures_inputs = []
    st.markdown("**Futures (SOFR, quarterly IMM)**")
    for i in range(num_futures):
        t_start       = round(DEPO_MATURITY + i * 0.25, 4)
        t_end         = round(t_start + 0.25, 4)
        default_price = round(95.85 - i * 0.10, 2)
        price = st.number_input(
            f"Future {i + 1}  ({t_start:.2f}Y – {t_end:.2f}Y)",
            min_value=90.0, max_value=100.0,
            value=default_price, step=0.01, format="%.2f",
            key=f"fut_{i}",
        )
        futures_inputs.append({
            "type":             InstrumentType.FUTURE,
            "startTime":        t_start,
            "maturity":         t_end,
            "price":            price,
            "paymentFrequency": 4,
        })

    st.markdown("**Vanilla Swaps**")
    swap_3y = st.slider("3Y Swap Rate", 0.01, 0.10, 0.0490, step=0.0005, format="%.4f")
    swap_5y = st.slider("5Y Swap Rate", 0.01, 0.10, 0.0520, step=0.0005, format="%.4f")

    dv01_tenor = st.selectbox(
        "DV01 target tenor", [2, 3, 5, 7, 10], index=2, format_func=lambda x: f"{x}Y"
    )

    market_data = [
        {
            "type":             InstrumentType.DEPOSIT,
            "startTime":        0.0,
            "maturity":         DEPO_MATURITY,
            "rate":             depo_rate,
            "paymentFrequency": 4,
        },
        *futures_inputs,
        {
            "type":             InstrumentType.SWAP,
            "startTime":        0.0,
            "maturity":         3.0,
            "rate":             swap_3y,
            "paymentFrequency": 2,
        },
        {
            "type":             InstrumentType.SWAP,
            "startTime":        0.0,
            "maturity":         5.0,
            "rate":             swap_5y,
            "paymentFrequency": 2,
        },
    ]

# ---------------------------------------------------------------------------
# Calibrate and display (right column)
# ---------------------------------------------------------------------------
with col_charts:
    try:
        result = calibrate_curve(market_data, OIS_NODES,
                                 dv01_maturity=float(dv01_tenor), dv01_frequency=2)

        # --- Forward zero curve ---
        st.subheader("Forward Zero Curve")
        df_fwd = pd.DataFrame({
            "Maturity (Y)":  result["times"],
            "Zero Rate (%)": [r * 100.0 for r in result["rates"]],
        })
        fig_fwd = px.line(
            df_fwd, x="Maturity (Y)", y="Zero Rate (%)", markers=True,
            title="Calibrated IBOR Forward Curve (multi-regime spline)"
        )
        fig_fwd.update_layout(hovermode="x unified")
        st.plotly_chart(fig_fwd, use_container_width=True)

        # --- OIS-IBOR basis spread ---
        if result["basis_times"]:
            st.subheader("OIS–IBOR Basis Spread")
            df_basis = pd.DataFrame({
                "Maturity (Y)":  result["basis_times"],
                "Spread (bps)":  result["basis_spreads_bps"],
            })
            fig_basis = px.bar(
                df_basis, x="Maturity (Y)", y="Spread (bps)",
                title="IBOR Forward Zero − OIS Zero Spread",
                color="Spread (bps)", color_continuous_scale="RdYlGn_r",
            )
            fig_basis.update_layout(coloraxis_showscale=False)
            st.plotly_chart(fig_basis, use_container_width=True)

        # --- Key-rate DV01 ---
        type_labels = {
            InstrumentType.DEPOSIT:  "DEPO",
            InstrumentType.FUTURE:   "FUT",
            InstrumentType.SWAP:     "SWAP",
            InstrumentType.OIS_SWAP: "OIS",
        }
        inst_labels = [
            f"{type_labels.get(inst['type'], '?')} {inst['maturity']:.2f}Y"
            for inst in market_data
        ]
        kr_dv01_bps = [v * 1e4 for v in result["key_rate_dv01"]]   # bps per 1bp bump

        st.subheader(f"Key-Rate DV01  ({dv01_tenor}Y par swap, per 1 bp bump)")
        par_dv01_bps = result["parallel_dv01"] * 1e4
        st.metric("Parallel DV01", f"{par_dv01_bps:.4f} bp / bp")

        df_dv01 = pd.DataFrame({
            "Instrument":  inst_labels,
            "DV01 (bp/bp)": kr_dv01_bps,
        })
        fig_dv01 = px.bar(
            df_dv01, x="Instrument", y="DV01 (bp/bp)",
            title=f"Key-Rate DV01 — sensitivity of {dv01_tenor}Y par rate to each instrument",
            color="DV01 (bp/bp)", color_continuous_scale="Blues",
        )
        fig_dv01.update_layout(coloraxis_showscale=False)
        st.plotly_chart(fig_dv01, use_container_width=True)

        # --- Numerical tables ---
        with st.expander("Zero Curve Nodes", expanded=False):
            st.dataframe(
                df_fwd.style.format({
                    "Maturity (Y)":  "{:.3f}",
                    "Zero Rate (%)": "{:.4f}",
                }),
                use_container_width=True,
            )

        with st.expander("Key-Rate DV01 Table", expanded=False):
            st.dataframe(
                df_dv01.style.format({"DV01 (bp/bp)": "{:.6f}"}),
                use_container_width=True,
            )

    except Exception as e:
        st.error(f"Calibration error: {e}")
        import traceback
        st.code(traceback.format_exc())
