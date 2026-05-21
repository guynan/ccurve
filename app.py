import streamlit as st
import pandas as pd
import plotly.express as px
from curve_bridge import calibrate_curve, InstrumentType

st.set_page_config(page_title="Dual-Curve OIS Calibration Dashboard", layout="wide")
st.title("📊 Fixed Income Dual-Curve Engine Interface")
st.caption("C99 High-Performance Bootstrap Backend running under Python C-Bridge Middleware")

# Setup Default Shared Static Baseline OIS Nodes
ois_nodes = [
    { "time": 0.25, "rate": 0.0350 }, { "time": 0.50, "rate": 0.0365 },
    { "time": 0.75, "rate": 0.0375 }, { "time": 1.00, "rate": 0.0385 },
    { "time": 2.00, "rate": 0.0400 }, { "time": 3.00, "rate": 0.0415 },
    { "time": 5.00, "rate": 0.0430 }
]

col1, col2 = st.columns([1, 2])

with col1:
    st.subheader("🔧 Live Instrument Parameter Configurations")
    
    # Real-Time User Input Modifiers
    depo_rate = st.slider("Deposit Rate (3M)", 0.01, 0.08, 0.0400, step=0.0005, format="%.4f")
    fut_price_1 = st.number_input("Future 1 Price (T=0.60)", min_value=90.0, max_value=100.0, value=95.85, step=0.01)
    fut_price_2 = st.number_input("Future 2 Price (T=0.85)", min_value=90.0, max_value=100.0, value=95.75, step=0.01)
    fut_price_3 = st.number_input("Future 3 Price (T=1.10)", min_value=90.0, max_value=100.0, value=95.60, step=0.01)
    swap_3y = st.slider("3Y Swap Fixed Rate", 0.01, 0.08, 0.0490, step=0.0005, format="%.4f")
    swap_5y = st.slider("5Y Swap Fixed Rate", 0.01, 0.08, 0.0520, step=0.0005, format="%.4f")

    # Map the web adjustments directly into the target instrument dictionary matrix
    market_data = [
        {"type": InstrumentType.DEPOSIT, "startTime": 0.00, "maturity": 0.25, "rate": depo_rate, "paymentFrequency": 4},
        {"type": InstrumentType.FUTURE, "startTime": 0.35, "maturity": 0.60, "price": fut_price_1, "paymentFrequency": 4},
        {"type": InstrumentType.FUTURE, "startTime": 0.60, "maturity": 0.85, "price": fut_price_2, "paymentFrequency": 4},
        {"type": InstrumentType.FUTURE, "startTime": 0.85, "maturity": 1.10, "price": fut_price_3, "paymentFrequency": 4},
        {"type": InstrumentType.SWAP, "startTime": 0.00, "maturity": 3.00, "rate": swap_3y, "paymentFrequency": 2},
        {"type": InstrumentType.SWAP, "startTime": 0.00, "maturity": 5.00, "rate": swap_5y, "paymentFrequency": 1},
    ]

with col2:
    st.subheader("📈 Calibrated Forward Curve Output")
    
    # ⚡ EXECUTING THE BRIDGE: Run the execution code via ctypes
    try:
        times, zero_rates = calibrate_curve(market_data, ois_nodes)
        
        # Turn into Pandas DataFrame for presentation charts
        df_chart = pd.DataFrame({
            "Maturity (Years)": times,
            "Calibrated Zero Rate (%)": [r * 100.0 for r in zero_rates]
        })
        
        # Render clean Plotly visualization canvas
        fig = px.line(df_chart, x="Maturity (Years)", y="Calibrated Zero Rate (%)", markers=True,
                      title="Calibrated Forward Projection Yield Curve (Multi-Regime Splined)")
        fig.update_layout(hovermode="x unified")
        st.plotly_chart(fig, use_container_width=True)
        
        st.subheader("📋 Numerical Output Tensor Data")
        st.dataframe(df_chart.style.format({"Maturity (Years)": "{:.2f}", "Calibrated Zero Rate (%)": "{:.4f}%"}), use_container_width=True)
        
    except Exception as e:
        st.error(f"Engine Core Disconnection Error: {e}")
