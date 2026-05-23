import streamlit as st
import pandas as pd
import plotly.graph_objects as go
from curve_bridge import calibrate_curve, InstrumentType

st.set_page_config(page_title="Dual-Curve OIS Calibration Dashboard", layout="wide")
st.title("Fixed Income Dual-Curve Engine Interface")
st.caption("C99 bootstrap backend · Python ctypes bridge")

# ── OIS curve nodes (used as the discount curve) ───────────────────────────
ois_nodes = [
    {"time": 0.25,  "rate": 0.0350},
    {"time": 0.50,  "rate": 0.0365},
    {"time": 0.75,  "rate": 0.0375},
    {"time": 1.00,  "rate": 0.0385},
    {"time": 2.00,  "rate": 0.0400},
    {"time": 3.00,  "rate": 0.0415},
    {"time": 5.00,  "rate": 0.0430},
    {"time": 7.00,  "rate": 0.0445},
    {"time": 10.0,  "rate": 0.0455},
]

def _interp_ois(t):
    """Piecewise-linear interpolation of OIS zero rate at time t."""
    nodes = ois_nodes
    if t <= nodes[0]["time"]:
        return nodes[0]["rate"]
    if t >= nodes[-1]["time"]:
        return nodes[-1]["rate"]
    for i in range(len(nodes) - 1):
        t0, t1 = nodes[i]["time"], nodes[i + 1]["time"]
        if t0 <= t <= t1:
            w = (t - t0) / (t1 - t0)
            return nodes[i]["rate"] * (1 - w) + nodes[i + 1]["rate"] * w
    return nodes[-1]["rate"]

# ── Sidebar: instrument inputs ─────────────────────────────────────────────
col1, col2 = st.columns([1, 2])

with col1:
    st.subheader("Instrument Inputs")

    depo_rate = st.slider("Deposit Rate (3M)", 0.01, 0.08, 0.0400,
                          step=0.0005, format="%.4f")

    num_futures = st.slider("Number of Futures", 1, 8, 3)

    fut_prices = []
    fut_starts = []
    fut_ends   = []
    base_start = 0.25  # first future starts at deposit maturity
    for i in range(num_futures):
        t_start = base_start + i * 0.25
        t_end   = t_start + 0.25
        fut_starts.append(t_start)
        fut_ends.append(t_end)
        default_price = round(95.85 - i * 0.10, 2)
        price = st.number_input(
            f"Future {i + 1}  ({t_start:.2f}Y – {t_end:.2f}Y)",
            min_value=90.0, max_value=100.0,
            value=default_price, step=0.01,
            key=f"fut_{i}",
        )
        fut_prices.append(price)

    st.divider()

    swap_2y  = st.slider("2Y Swap Rate",  0.01, 0.08, 0.0450, step=0.0005, format="%.4f")
    swap_3y  = st.slider("3Y Swap Rate",  0.01, 0.08, 0.0490, step=0.0005, format="%.4f")
    swap_5y  = st.slider("5Y Swap Rate",  0.01, 0.08, 0.0520, step=0.0005, format="%.4f")
    swap_7y  = st.slider("7Y Swap Rate",  0.01, 0.08, 0.0540, step=0.0005, format="%.4f")
    swap_10y = st.slider("10Y Swap Rate", 0.01, 0.08, 0.0560, step=0.0005, format="%.4f")

    # Build instrument list
    market_data = [
        {"type": InstrumentType.DEPOSIT, "startTime": 0.00, "maturity": 0.25,
         "rate": depo_rate, "paymentFrequency": 4},
    ]
    for i in range(num_futures):
        market_data.append({
            "type": InstrumentType.FUTURE,
            "startTime": fut_starts[i], "maturity": fut_ends[i],
            "price": fut_prices[i], "paymentFrequency": 4,
        })
    for mat, rate in [
        (2.0, swap_2y), (3.0, swap_3y), (5.0, swap_5y),
        (7.0, swap_7y), (10.0, swap_10y),
    ]:
        market_data.append({
            "type": InstrumentType.SWAP, "startTime": 0.00,
            "maturity": mat, "rate": rate, "paymentFrequency": 2,
        })
    # Sort by maturity so the bootstrap receives instruments in strictly
    # ascending order regardless of how futures and swaps interleave.
    # When a future and a swap share the same maturity, keep the swap
    # (more information: par rate anchors the curve better than a futures price).
    market_data.sort(key=lambda x: (x["maturity"], x["type"] == InstrumentType.FUTURE))
    seen_maturities: set = set()
    deduped = []
    for inst in market_data:
        m = inst["maturity"]
        if m not in seen_maturities:
            seen_maturities.add(m)
            deduped.append(inst)
    market_data = deduped

# ── Main panel: calibrated curves ─────────────────────────────────────────
with col2:
    try:
        times, zero_rates = calibrate_curve(market_data, ois_nodes)

        ois_at_fwd = [_interp_ois(t) for t in times]
        basis_bps  = [(zr - oir) * 10_000
                      for zr, oir in zip(zero_rates, ois_at_fwd)]

        tab_curves, tab_basis, tab_data = st.tabs(
            ["Yield Curves", "Basis Spread", "Node Data"]
        )

        with tab_curves:
            fig = go.Figure()
            fig.add_trace(go.Scatter(
                x=times,
                y=[r * 100 for r in zero_rates],
                mode="lines+markers",
                name="IBOR Forward",
                line=dict(color="#1f77b4"),
            ))
            fig.add_trace(go.Scatter(
                x=[n["time"] for n in ois_nodes],
                y=[n["rate"] * 100 for n in ois_nodes],
                mode="lines+markers",
                name="OIS Discount",
                line=dict(color="#ff7f0e", dash="dash"),
            ))
            fig.update_layout(
                title="IBOR Forward Curve vs OIS Discount Curve",
                xaxis_title="Maturity (Years)",
                yaxis_title="Zero Rate (%)",
                hovermode="x unified",
                legend=dict(x=0.02, y=0.98),
            )
            st.plotly_chart(fig, use_container_width=True)

        with tab_basis:
            fig2 = go.Figure()
            fig2.add_trace(go.Bar(
                x=times,
                y=basis_bps,
                name="IBOR–OIS Basis",
                marker_color="steelblue",
            ))
            fig2.update_layout(
                title="IBOR–OIS Basis Spread",
                xaxis_title="Maturity (Years)",
                yaxis_title="Spread (bps)",
                hovermode="x unified",
            )
            st.plotly_chart(fig2, use_container_width=True)

        with tab_data:
            df = pd.DataFrame({
                "Maturity (Y)":    times,
                "IBOR Zero (%)":   [r * 100 for r in zero_rates],
                "OIS Zero (%)":    [r * 100 for r in ois_at_fwd],
                "Basis (bps)":     basis_bps,
            })
            st.dataframe(
                df.style.format({
                    "Maturity (Y)":   "{:.3f}",
                    "IBOR Zero (%)":  "{:.4f}",
                    "OIS Zero (%)":   "{:.4f}",
                    "Basis (bps)":    "{:.2f}",
                }),
                use_container_width=True,
            )

    except Exception as e:
        st.error(f"Calibration error: {e}")
        st.exception(e)
