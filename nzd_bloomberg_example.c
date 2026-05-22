/*
 * nzd_bloomberg_example.c
 *
 * Demonstrates bootstrapping an NZD BKBM interest rate curve using live
 * market data fetched from Bloomberg.  The curve is single-curve
 * (self-discounted) which is standard practice for NZD: there is no
 * liquid overnight-indexed swap market analogous to USD SOFR OIS.
 *
 * Instruments used:
 *   NDBB3M Curncy              — 3-month NZD bank bill (BKBM deposit)
 *   ZB1–ZB4 Comdty             — ASX 90-day bank bill futures (quarterly)
 *   NDSWAP{2,3,4,5,6,7,10,12,15} Curncy — NZD quarterly IRS (BKBM float)
 *
 * Build (requires Bloomberg C++ SDK at BLPAPI_HOME and built shared libs):
 *   make bloomberg
 *   make nzd_example
 *
 * Run:
 *   ./nzd_example [YYYY-MM-DD] [blp-host] [blp-port]
 *
 *   Defaults: today's date, localhost, 8194.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dual_curve.h"
#include "blpapi_fetcher.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *inst_type_name(InstrumentType t)
{
    switch (t) {
    case DEPOSIT:  return "DEPOSIT";
    case FUTURE:   return "FUTURE ";
    case SWAP:     return "SWAP   ";
    case OIS_SWAP: return "OIS    ";
    default:       return "UNKNOWN";
    }
}

static void print_separator(void)
{
    printf("  %s\n", "------------------------------------------------------------");
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *as_of_date = "2026-05-22";
    const char *blp_host   = "localhost";
    int         blp_port   = 8194;

    if (argc > 1) as_of_date = argv[1];
    if (argc > 2) blp_host   = argv[2];
    if (argc > 3) blp_port   = atoi(argv[3]);

    printf("================================================================\n");
    printf("  NZD BKBM Curve Bootstrap  —  Bloomberg Data Feed\n");
    printf("================================================================\n");
    printf("  Anchor date : %s\n", as_of_date);
    printf("  Bloomberg   : %s:%d\n\n", blp_host, blp_port);

    /* ----------------------------------------------------------------
     * 1. Connect to Bloomberg
     * ---------------------------------------------------------------- */
    BlpSession *session = blp_session_create(blp_host, blp_port);
    if (!blp_session_connected(session)) {
        fprintf(stderr,
            "Error: cannot connect to Bloomberg at %s:%d.\n"
            "       Ensure the Bloomberg API server / B-PIPE is running.\n",
            blp_host, blp_port);
        blp_session_destroy(session);
        return 1;
    }
    printf("  Bloomberg session: connected\n\n");

    /* ----------------------------------------------------------------
     * 2. Fetch NZD market instruments
     * ---------------------------------------------------------------- */
    MarketInstrument instruments[MAX_NODES];
    memset(instruments, 0, sizeof(instruments));

    int n = blp_fetch_nzd_curve_instruments(
                session, instruments, MAX_NODES, as_of_date);

    blp_session_destroy(session);   /* session no longer needed */

    if (n <= 0) {
        fprintf(stderr,
            "Error: blp_fetch_nzd_curve_instruments returned %d.\n"
            "       Check that all NZD tickers are available on your terminal.\n",
            n);
        return 1;
    }

    printf("  Fetched %d instruments from Bloomberg:\n\n", n);
    printf("  %-4s  %-25s  %-8s  %-8s  %-10s\n",
           "Idx", "Ticker (type)", "Start Y", "End Y", "Rate / Price");
    print_separator();
    for (int i = 0; i < n; i++) {
        const MarketInstrument *inst = &instruments[i];
        if (inst->type == FUTURE)
            printf("  [%2d]  %-7s               %6.3f   %6.3f   price=%.4f\n",
                   i, inst_type_name(inst->type),
                   inst->startTime, inst->maturity, inst->price);
        else
            printf("  [%2d]  %-7s  mat=%5.2fY               rate=%.4f%%\n",
                   i, inst_type_name(inst->type),
                   inst->maturity, inst->rate * 100.0);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 3. Bootstrap the NZD single curve
     *
     *    bootstrapOisCurve() wraps bootstrapCurve(curve, curve, ...)
     *    — the forward projection curve and the discount curve are the
     *    same object (self-discounted single-curve bootstrap), which is
     *    the standard approach for NZD BKBM.
     * ---------------------------------------------------------------- */
    InterestRateCurve nzdCurve;
    memset(&nzdCurve, 0, sizeof(nzdCurve));

    printf("  Bootstrapping NZD single curve ...\n");
    int rc = bootstrapOisCurve(&nzdCurve, instruments, n, NULL);
    if (rc != 0) {
        fprintf(stderr,
            "Error: bootstrapOisCurve failed (rc=%d).\n"
            "       Verify instrument ordering and that no maturity is missing.\n",
            rc);
        return 1;
    }
    printf("  Done. %d zero-rate nodes calibrated.\n\n", nzdCurve.numNodes);

    /* ----------------------------------------------------------------
     * 4. Print the calibrated zero curve
     * ---------------------------------------------------------------- */
    printf("  NZD Calibrated Zero Curve (continuous, Act/365)\n");
    print_separator();
    printf("  %-4s  %-8s  %-8s  %-12s  %-12s\n",
           "Idx", "Type", "Mat (Y)", "Zero Rate %", "Disc Factor");
    print_separator();

    for (int i = 0; i < nzdCurve.numNodes; i++) {
        const char *lbl = (i < n) ? inst_type_name(instruments[i].type) : "NODE   ";
        printf("  [%2d]  %-7s  %6.3f   %10.4f%%   %.8f\n",
               i, lbl,
               nzdCurve.times[i],
               nzdCurve.rates[i] * 100.0,
               nzdCurve.dfs[i]);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 5. Verify calibration: par swap rates should reproduce market quotes
     * ---------------------------------------------------------------- */
    printf("  Par Swap Rate Verification (quarterly, Act/365)\n");
    print_separator();
    printf("  %-8s  %-12s  %-12s  %-10s\n",
           "Tenor", "Par Rate %", "Market %", "Error (bp)");
    print_separator();

    static const double SWAP_TENORS[] = { 2.0, 3.0, 4.0, 5.0,
                                           6.0, 7.0, 10.0, 12.0, 15.0 };
    int nTenors = (int)(sizeof(SWAP_TENORS) / sizeof(SWAP_TENORS[0]));

    for (int ti = 0; ti < nTenors; ti++) {
        double par = solveParSwapRate(&nzdCurve, &nzdCurve,
                                      SWAP_TENORS[ti], 4 /* quarterly */);
        /* find matching market quote */
        double mkt = 0.0;
        for (int j = 0; j < n; j++) {
            if (instruments[j].type == SWAP &&
                fabs(instruments[j].maturity - SWAP_TENORS[ti]) < 0.01) {
                mkt = instruments[j].rate;
                break;
            }
        }
        double err_bp = (mkt > 0.0) ? (par - mkt) * 1e4 : 0.0;
        printf("  %5.1fY    %10.4f%%   %10s   %+8.4f\n",
               SWAP_TENORS[ti],
               par * 100.0,
               (mkt > 0.0) ? "" : "  (no quote)",   /* blank if market quote missing */
               err_bp);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 6. Implied forward rates across key periods
     * ---------------------------------------------------------------- */
    printf("  Implied Forward Rates\n");
    print_separator();
    printf("  %-12s  %-12s  %-12s\n", "Period start", "Period end", "Fwd rate %");
    print_separator();

    static const struct { double s; double e; } FWD_PAIRS[] = {
        { 0.00, 0.25 },   /* ON to 3M  */
        { 0.25, 0.50 },   /* 3M  to 6M  — ZB1 period */
        { 0.50, 0.75 },   /* 6M  to 9M  — ZB2 period */
        { 0.75, 1.00 },   /* 9M  to 12M — ZB3 period */
        { 1.00, 1.25 },   /* 12M to 15M — ZB4 period */
        { 1.25, 2.00 },   /* tail to 2Y */
        { 2.00, 3.00 },   /* 2Y–3Y      */
        { 3.00, 5.00 },   /* 3Y–5Y      */
        { 5.00, 7.00 },   /* 5Y–7Y      */
        { 7.00, 10.00 },  /* 7Y–10Y     */
        { 10.00, 15.00 }, /* 10Y–15Y    */
    };
    int nFwd = (int)(sizeof(FWD_PAIRS) / sizeof(FWD_PAIRS[0]));

    for (int i = 0; i < nFwd; i++) {
        double fwd = getForwardRate(&nzdCurve,
                                    FWD_PAIRS[i].s, FWD_PAIRS[i].e);
        printf("  %7.2fY      %7.2fY      %10.4f%%\n",
               FWD_PAIRS[i].s, FWD_PAIRS[i].e, fwd * 100.0);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 7. Key-rate DV01 for the 5Y par swap (vs each calibration instrument)
     * ---------------------------------------------------------------- */
    printf("  Key-Rate DV01  (5Y par swap, per +1bp on each instrument)\n");
    print_separator();
    printf("  %-4s  %-8s  %-8s  %-12s\n",
           "Idx", "Type", "Mat (Y)", "DV01 (bp/bp)");
    print_separator();

    double dv01[MAX_NODES];
    computeKeyRateDV01(instruments, n, &nzdCurve, 5.0, 4, dv01);

    double sum_dv01 = 0.0;
    for (int i = 0; i < n; i++) {
        const char *lbl = inst_type_name(instruments[i].type);
        printf("  [%2d]  %-7s  %6.3f   %+12.6f\n",
               i, lbl, instruments[i].maturity, dv01[i] * 1e4);
        sum_dv01 += dv01[i] * 1e4;
    }
    print_separator();
    printf("  Sum (≈ parallel DV01): %+.6f bp/bp\n\n", sum_dv01);

    printf("================================================================\n");
    return 0;
}
