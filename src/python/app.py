import streamlit as st
import pandas as pd
import plotly.graph_objects as go
from curve_bridge import calibrate_curve, InstrumentType
from xccy_bridge import XCCYBridge

st.set_page_config(page_title="Dual-Curve OIS Calibration Dashboard", layout="wide")
st.title("Fixed Income Dual-Curve Engine Interface")
st.caption("C99 bootstrap backend · Python ctypes bridge")

top_tab_single, top_tab_xccy = st.tabs(
    ["Single-Currency Bootstrap", "Cross-Currency"]
)


# ═══════════════════════════════════════════════════════════════════════════
# Tab 1 · single-currency dual-curve bootstrap (existing behaviour)
# ═══════════════════════════════════════════════════════════════════════════

with top_tab_single:
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

    col1, col2 = st.columns([1, 2])

    with col1:
        st.subheader("Instrument Inputs")

        depo_rate = st.slider("Deposit Rate (3M)", 0.01, 0.08, 0.0400,
                              step=0.0005, format="%.4f")

        num_futures = st.slider("Number of Futures", 1, 8, 3)

        fut_prices = []
        fut_starts = []
        fut_ends   = []
        base_start = 0.25
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
        market_data.sort(key=lambda x: (x["maturity"], x["type"] == InstrumentType.FUTURE))
        seen_maturities: set = set()
        deduped = []
        for inst in market_data:
            m = inst["maturity"]
            if m not in seen_maturities:
                seen_maturities.add(m)
                deduped.append(inst)
        market_data = deduped

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


# ═══════════════════════════════════════════════════════════════════════════
# Tab 2 · Cross-currency basis swap pricer
#
# FX swaps at the front of a *foreign* IBOR curve imply DF_for(T) from
# DF_dom(T) via forward/spot; longer-dated foreign IBOR/OIS points come
# from swap quotes.  With both currencies calibrated we then solve for
# fair XCCY basis spreads and price a specific trade.
# ═══════════════════════════════════════════════════════════════════════════

with top_tab_xccy:
    st.subheader("Cross-Currency Basis Swap")
    st.caption(
        "Domestic curve is bootstrapped from deposit + swaps. Foreign curve "
        "uses FX-swap forwards for the short end and IBOR swap quotes further out."
    )

    dom_col, for_col, trade_col = st.columns([1, 1, 1])

    # ── Domestic (default USD SOFR) ────────────────────────────────────────
    with dom_col:
        st.markdown("**Domestic curve** (e.g. USD)")
        dom_label   = st.text_input("Domestic label", "USD", key="xccy_dom_label")
        dom_depo    = st.slider("Domestic 3M deposit",
                                0.00, 0.08, 0.0430, step=0.0005,
                                format="%.4f", key="xccy_dom_depo")
        dom_swap_2y = st.slider("Domestic 2Y swap",
                                0.00, 0.08, 0.0400, step=0.0005,
                                format="%.4f", key="xccy_dom_2y")
        dom_swap_5y = st.slider("Domestic 5Y swap",
                                0.00, 0.08, 0.0410, step=0.0005,
                                format="%.4f", key="xccy_dom_5y")
        dom_swap_10y = st.slider("Domestic 10Y swap",
                                 0.00, 0.08, 0.0440, step=0.0005,
                                 format="%.4f", key="xccy_dom_10y")

    # ── Foreign (default EUR) ──────────────────────────────────────────────
    with for_col:
        st.markdown("**Foreign curve** (e.g. EUR) — FX-swap front end")
        for_label = st.text_input("Foreign label", "EUR", key="xccy_for_label")
        fx_spot   = st.number_input("FX spot (dom per for)",
                                    value=1.0800, step=0.0001, format="%.4f",
                                    key="xccy_fx_spot")

        st.caption("FX forwards — outright (dom per for). "
                   "Defaults follow covered-interest parity against the "
                   "log-linear discount curves, so the FX-implied front end "
                   "joins smoothly to the par swap rates. USD > EUR ⇒ EUR at "
                   "forward premium (F < S).")
        fx_fwd_3m = st.number_input("3M forward", value=1.0752, step=0.0001,
                                    format="%.4f", key="xccy_fwd_3m")
        fx_fwd_6m = st.number_input("6M forward", value=1.0721, step=0.0001,
                                    format="%.4f", key="xccy_fwd_6m")
        fx_fwd_1y = st.number_input("1Y forward",
                                    value=1.0659,
                                    step=0.0001,
                                    format="%.4f",
                                    key="xccy_fwd_1y")

        st.caption("Foreign IBOR par swap rates (used past 1Y)")
        for_swap_2y = st.slider("Foreign 2Y swap",
                                0.00, 0.08, 0.0262, step=0.0005,
                                format="%.4f", key="xccy_for_2y")
        for_swap_5y = st.slider("Foreign 5Y swap",
                                0.00, 0.08, 0.0318, step=0.0005,
                                format="%.4f", key="xccy_for_5y")
        for_swap_10y = st.slider("Foreign 10Y swap",
                                 0.00, 0.08, 0.0350, step=0.0005,
                                 format="%.4f", key="xccy_for_10y")

    # ── Trade parameters ───────────────────────────────────────────────────
    with trade_col:
        st.markdown("**XCCY trade**")
        trade_tenor    = st.slider("Trade tenor (Y)",
                                   1.0, 10.0, 5.0, step=1.0,
                                   key="xccy_trade_tenor")
        dom_notional   = st.number_input("Domestic notional",
                                         value=10_000_000.0,
                                         step=1_000_000.0, format="%.2f",
                                         key="xccy_notional")
        traded_bps     = st.number_input("Quoted basis (bps, on dom leg)",
                                         value=-20.0, step=0.5, format="%.2f",
                                         key="xccy_quoted_bps")
        pay_freq       = st.selectbox("Payment frequency",
                                      options=[4, 2],
                                      format_func=lambda f: "Quarterly" if f == 4 else "Semi-Annual",
                                      key="xccy_freq")
        resettable_ui  = st.checkbox("Resettable (MtM) notional",
                                     value=False, key="xccy_reset")

    st.divider()

    # ── Build & calibrate both curves ─────────────────────────────────────
    try:
        # Domestic OIS discount curve — simple flat/interpolated spec.
        # In a full multi-currency setup you'd calibrate this from OIS quotes;
        # here we approximate with rates parallel to the IBOR swap quotes for demo.
        dom_ois_nodes = [
            {"time": 0.25, "rate": dom_depo},
            {"time": 2.00, "rate": dom_swap_2y - 0.0005},
            {"time": 5.00, "rate": dom_swap_5y - 0.0005},
            {"time": 10.0, "rate": dom_swap_10y - 0.0005},
        ]
        for_ois_nodes = [
            {"time": 0.25, "rate": for_swap_2y - 0.0050},
            {"time": 2.00, "rate": for_swap_2y - 0.0007},
            {"time": 5.00, "rate": for_swap_5y - 0.0007},
            {"time": 10.0, "rate": for_swap_10y - 0.0008},
        ]

        # Domestic IBOR forward curve
        dom_instruments = [
            {"type": InstrumentType.DEPOSIT, "startTime": 0.00,
             "maturity": 0.25, "rate": dom_depo, "paymentFrequency": 4},
            {"type": InstrumentType.SWAP, "startTime": 0.00,
             "maturity": 2.0,  "rate": dom_swap_2y,  "paymentFrequency": 2},
            {"type": InstrumentType.SWAP, "startTime": 0.00,
             "maturity": 5.0,  "rate": dom_swap_5y,  "paymentFrequency": 2},
            {"type": InstrumentType.SWAP, "startTime": 0.00,
             "maturity": 10.0, "rate": dom_swap_10y, "paymentFrequency": 2},
        ]
        dom_fwd_times, dom_fwd_rates = calibrate_curve(dom_instruments, dom_ois_nodes)
        dom_fwd_nodes = [{"time": t, "rate": r}
                         for t, r in zip(dom_fwd_times, dom_fwd_rates)]

        # Foreign IBOR forward curve
        #   Front end: FX_SWAP instruments (rate = spot, price = forward outright)
        #   Long end : par swap quotes
        for_instruments = [
            {"type": InstrumentType.FX_SWAP, "startTime": 0.00,
             "maturity": 0.25, "rate": fx_spot, "price": fx_fwd_3m,
             "paymentFrequency": 1},
            {"type": InstrumentType.FX_SWAP, "startTime": 0.00,
             "maturity": 0.50, "rate": fx_spot, "price": fx_fwd_6m,
             "paymentFrequency": 1},
            {"type": InstrumentType.FX_SWAP, "startTime": 0.00,
             "maturity": 1.00, "rate": fx_spot, "price": fx_fwd_1y,
             "paymentFrequency": 1},
            {"type": InstrumentType.SWAP, "startTime": 0.00,
             "maturity": 2.0,  "rate": for_swap_2y,  "paymentFrequency": 2},
            {"type": InstrumentType.SWAP, "startTime": 0.00,
             "maturity": 5.0,  "rate": for_swap_5y,  "paymentFrequency": 2},
            {"type": InstrumentType.SWAP, "startTime": 0.00,
             "maturity": 10.0, "rate": for_swap_10y, "paymentFrequency": 2},
        ]

        # Foreign IBOR forward calibration uses the foreign OIS as discount.
        # FX_SWAP inside the C bootstrap treats *disc* as the domestic reference
        # to imply DF_for(T) — so pass the domestic OIS curve as `ois_nodes`
        # here, because we want DF_for = DF_dom * S / F at the front.
        for_fwd_times, for_fwd_rates = calibrate_curve(for_instruments, dom_ois_nodes)
        for_fwd_nodes = [{"time": t, "rate": r}
                         for t, r in zip(for_fwd_times, for_fwd_rates)]

        # ── XCCY pricing ──────────────────────────────────────────────────
        bridge = XCCYBridge()

        tenors_ladder = [1.0, 2.0, 3.0, 5.0, 7.0, 10.0]
        ladder = bridge.tenor_ladder(
            tenors        = tenors_ladder,
            frequency     = int(pay_freq),
            dom_notional  = dom_notional,
            fx_spot       = fx_spot,
            resettable    = 1 if resettable_ui else 0,
            dom_fwd_nodes = dom_fwd_nodes,
            dom_ois_nodes = dom_ois_nodes,
            for_fwd_nodes = for_fwd_nodes,
            for_ois_nodes = for_ois_nodes,
        )

        price = bridge.price_xccy_swap(
            maturity         = float(trade_tenor),
            frequency        = int(pay_freq),
            dom_notional     = dom_notional,
            fx_spot          = fx_spot,
            basis_spread_bps = float(traded_bps),
            resettable       = 1 if resettable_ui else 0,
            dom_fwd_nodes    = dom_fwd_nodes,
            dom_ois_nodes    = dom_ois_nodes,
            for_fwd_nodes    = for_fwd_nodes,
            for_ois_nodes    = for_ois_nodes,
        )

        curves_tab, ladder_tab, trade_tab = st.tabs(
            ["Calibrated Curves", "Fair-Spread Ladder", "Trade P&L"]
        )

        with curves_tab:
            fig = go.Figure()
            fig.add_trace(go.Scatter(
                x=dom_fwd_times, y=[r * 100 for r in dom_fwd_rates],
                mode="lines+markers", name=f"{dom_label} IBOR",
                line=dict(color="#1f77b4"),
            ))
            fig.add_trace(go.Scatter(
                x=[n["time"] for n in dom_ois_nodes],
                y=[n["rate"] * 100 for n in dom_ois_nodes],
                mode="lines+markers", name=f"{dom_label} OIS",
                line=dict(color="#1f77b4", dash="dash"),
            ))
            fig.add_trace(go.Scatter(
                x=for_fwd_times, y=[r * 100 for r in for_fwd_rates],
                mode="lines+markers", name=f"{for_label} IBOR",
                line=dict(color="#ff7f0e"),
            ))
            fig.add_trace(go.Scatter(
                x=[n["time"] for n in for_ois_nodes],
                y=[n["rate"] * 100 for n in for_ois_nodes],
                mode="lines+markers", name=f"{for_label} OIS",
                line=dict(color="#ff7f0e", dash="dash"),
            ))
            fig.update_layout(
                title=f"{dom_label} vs {for_label} — IBOR / OIS Zero Curves",
                xaxis_title="Maturity (Years)",
                yaxis_title="Zero Rate (%)",
                hovermode="x unified",
                legend=dict(x=0.02, y=0.98),
            )
            st.plotly_chart(fig, use_container_width=True)

        with ladder_tab:
            ladder_df = pd.DataFrame(ladder)
            ladder_df.columns = ["Tenor (Y)", "Fair Basis (bps)"]
            fig_l = go.Figure(go.Bar(
                x=ladder_df["Tenor (Y)"],
                y=ladder_df["Fair Basis (bps)"],
                marker_color="steelblue",
            ))
            fig_l.update_layout(
                title=f"Fair XCCY Basis — {dom_label} vs {for_label}"
                      f"  (spread on {dom_label} leg)",
                xaxis_title="Tenor (Y)",
                yaxis_title="Basis Spread (bps)",
                hovermode="x unified",
            )
            st.plotly_chart(fig_l, use_container_width=True)
            st.dataframe(
                ladder_df.style.format({
                    "Tenor (Y)":        "{:.1f}",
                    "Fair Basis (bps)": "{:+.2f}",
                }),
                use_container_width=True,
            )

        with trade_tab:
            m1, m2, m3 = st.columns(3)
            m1.metric("NPV (dom ccy)", f"{price.npv_dom_ccy:+,.2f}")
            m2.metric("Quoted Basis (bps)", f"{price.basis_spread_bps:+.2f}")
            m3.metric("PV Dom Leg", f"{price.pv_dom_leg:+,.2f}")

            m4, m5, m6 = st.columns(3)
            m4.metric("PV For Leg (dom ccy)", f"{price.pv_for_leg_dom:+,.2f}")
            m5.metric("DV01 Dom Curves", f"{price.dv01_dom:+,.2f}")
            m6.metric("DV01 For Curves", f"{price.dv01_for:+,.2f}")

            st.metric("FX Delta (per 1% FX move)", f"{price.fx_delta:+,.2f}")

    except Exception as e:
        st.error(f"XCCY calibration/pricing error: {e}")
        st.exception(e)
