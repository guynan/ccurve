/*
 * nzd_bloomberg_example.c
 *
 * Demonstrates dual-curve bootstrapping for NZD using live Bloomberg data.
 *
 * NZD has a liquid NZONIA OIS market, so the standard approach is:
 *   1. Bootstrap the NZONIA OIS discount curve from meeting-dated OIS swaps.
 *   2. Bootstrap the BKBM forward curve from quarterly IRS discounted off OIS.
 *
 * Instruments used:
 *   NDSF{1..6}A Curncy  — meeting-dated NZONIA OIS swaps  (OIS curve)
 *   NDSWAP{3,4,5,6,7,10,12,15} Curncy — quarterly BKBM IRS (IBOR curve)
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
    printf("  NZD Dual-Curve Bootstrap  --  Bloomberg Data Feed\n");
    printf("  NZONIA OIS + BKBM Forward Curve\n");
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
     * 2. Fetch all NZD market instruments (OIS + BKBM in one BDP call)
     * ---------------------------------------------------------------- */
    MarketInstrument all_instruments[MAX_NODES];
    memset(all_instruments, 0, sizeof(all_instruments));

    int n_all = blp_fetch_nzd_curve_instruments(
                    session, all_instruments, MAX_NODES, as_of_date);

    blp_session_destroy(session);

    if (n_all <= 0) {
        fprintf(stderr,
            "Error: blp_fetch_nzd_curve_instruments returned %d.\n"
            "       Check that all NZD tickers are available on your terminal.\n",
            n_all);
        return 1;
    }

    /* Separate into OIS (NZONIA) and IBOR (BKBM) arrays */
    MarketInstrument ois_instruments[MAX_NODES];
    MarketInstrument ibor_instruments[MAX_NODES];
    int n_ois = 0, n_ibor = 0;

    for (int i = 0; i < n_all; i++) {
        if (all_instruments[i].type == OIS_SWAP)
            ois_instruments[n_ois++] = all_instruments[i];
        else if (all_instruments[i].type == SWAP)
            ibor_instruments[n_ibor++] = all_instruments[i];
    }

    printf("  Fetched %d instruments (%d OIS, %d BKBM IRS):\n\n",
           n_all, n_ois, n_ibor);
    printf("  %-4s  %-8s  %-8s  %-10s\n", "Idx", "Type", "Mat (Y)", "Rate");
    print_separator();
    for (int i = 0; i < n_all; i++) {
        const MarketInstrument *inst = &all_instruments[i];
        printf("  [%2d]  %-7s  %6.3f   %8.4f%%\n",
               i, inst_type_name(inst->type),
               inst->maturity, inst->rate * 100.0);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 3. Bootstrap NZONIA OIS discount curve
     *    (self-discounted: uses oisCurve as its own discount curve)
     * ---------------------------------------------------------------- */
    InterestRateCurve oisCurve;
    memset(&oisCurve, 0, sizeof(oisCurve));

    printf("  Bootstrapping NZONIA OIS curve (%d instruments) ...\n", n_ois);
    int rc = bootstrapOisCurve(&oisCurve, ois_instruments, n_ois, NULL);
    if (rc != 0) {
        fprintf(stderr, "Error: bootstrapOisCurve failed (rc=%d).\n", rc);
        return 1;
    }
    printf("  Done. %d OIS nodes calibrated.\n\n", oisCurve.numNodes);

    /* ----------------------------------------------------------------
     * 4. Bootstrap BKBM forward curve (discounted off OIS)
     * ---------------------------------------------------------------- */
    InterestRateCurve bkbmCurve;
    memset(&bkbmCurve, 0, sizeof(bkbmCurve));

    printf("  Bootstrapping BKBM forward curve (%d instruments) ...\n", n_ibor);
    int rc2 = bootstrapCurve(&bkbmCurve, &oisCurve,
                              ibor_instruments, n_ibor, NULL);
    if (rc2 != 0) {
        fprintf(stderr, "Error: bootstrapCurve (BKBM) failed (rc=%d).\n", rc2);
        return 1;
    }
    printf("  Done. %d BKBM forward nodes calibrated.\n\n", bkbmCurve.numNodes);

    /* ----------------------------------------------------------------
     * 5. NZONIA OIS zero curve
     * ---------------------------------------------------------------- */
    printf("  NZONIA OIS Zero Curve (continuous, Act/365)\n");
    print_separator();
    printf("  %-4s  %-8s  %-12s  %-12s\n",
           "Idx", "Mat (Y)", "Zero Rate %", "Disc Factor");
    print_separator();
    for (int i = 0; i < oisCurve.numNodes; i++) {
        printf("  [%2d]  %6.3f   %10.4f%%   %.8f\n",
               i, oisCurve.times[i],
               oisCurve.rates[i] * 100.0,
               oisCurve.dfs[i]);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 6. BKBM forward zero curve
     * ---------------------------------------------------------------- */
    printf("  BKBM Forward Zero Curve (continuous, Act/365)\n");
    print_separator();
    printf("  %-4s  %-8s  %-12s  %-12s\n",
           "Idx", "Mat (Y)", "Zero Rate %", "Disc Factor");
    print_separator();
    for (int i = 0; i < bkbmCurve.numNodes; i++) {
        printf("  [%2d]  %6.3f   %10.4f%%   %.8f\n",
               i, bkbmCurve.times[i],
               bkbmCurve.rates[i] * 100.0,
               bkbmCurve.dfs[i]);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 7. OIS-BKBM basis spread
     * ---------------------------------------------------------------- */
    BasisCurve basis;
    memset(&basis, 0, sizeof(basis));
    computeBasisCurve(&bkbmCurve, &oisCurve, &basis);

    printf("  OIS-BKBM Basis Spread (BKBM zero minus NZONIA zero)\n");
    print_separator();
    printf("  %-8s  %-12s\n", "Mat (Y)", "Spread (bps)");
    print_separator();
    for (int i = 0; i < basis.numNodes; i++) {
        printf("  %6.3f   %+10.4f\n",
               basis.times[i], basis.spreadsBps[i]);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 8. Par BKBM swap rate verification
     *    Calibrated par rates should reproduce market quotes to < 0.01 bp
     * ---------------------------------------------------------------- */
    static const double SWAP_TENORS[] = { 3.0, 4.0, 5.0, 6.0,
                                           7.0, 10.0, 12.0, 15.0 };
    int nTenors = (int)(sizeof(SWAP_TENORS) / sizeof(SWAP_TENORS[0]));

    printf("  Par BKBM Swap Rate Verification (quarterly, Act/365)\n");
    print_separator();
    printf("  %-8s  %-12s  %-12s  %-10s\n",
           "Tenor", "Par Rate %", "Market %", "Error (bp)");
    print_separator();
    for (int ti = 0; ti < nTenors; ti++) {
        double par = solveParSwapRate(&bkbmCurve, &oisCurve,
                                      SWAP_TENORS[ti], 4 /* quarterly */);
        double mkt = 0.0;
        for (int j = 0; j < n_ibor; j++) {
            if (ibor_instruments[j].type == SWAP &&
                fabs(ibor_instruments[j].maturity - SWAP_TENORS[ti]) < 0.01) {
                mkt = ibor_instruments[j].rate;
                break;
            }
        }
        double err_bp = (mkt > 0.0) ? (par - mkt) * 1e4 : 0.0;
        printf("  %5.1fY    %10.4f%%   %10.4f%%   %+8.4f\n",
               SWAP_TENORS[ti], par * 100.0, mkt * 100.0, err_bp);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 9. Implied BKBM forward rates
     * ---------------------------------------------------------------- */
    printf("  Implied BKBM Forward Rates\n");
    print_separator();
    printf("  %-12s  %-12s  %-12s\n",
           "Period start", "Period end", "Fwd rate %");
    print_separator();

    static const struct { double s; double e; } FWD_PAIRS[] = {
        { 0.00,  0.25  },
        { 0.25,  0.50  },
        { 0.50,  1.00  },
        { 1.00,  2.00  },
        { 2.00,  3.00  },
        { 3.00,  5.00  },
        { 5.00,  7.00  },
        { 7.00,  10.00 },
        { 10.00, 15.00 },
    };
    int nFwd = (int)(sizeof(FWD_PAIRS) / sizeof(FWD_PAIRS[0]));

    for (int i = 0; i < nFwd; i++) {
        double fwd = getForwardRate(&bkbmCurve,
                                    FWD_PAIRS[i].s, FWD_PAIRS[i].e);
        printf("  %7.2fY      %7.2fY      %10.4f%%\n",
               FWD_PAIRS[i].s, FWD_PAIRS[i].e, fwd * 100.0);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 10. Key-rate DV01 for the 5Y BKBM par swap
     *     Sensitivity to each BKBM IRS calibration input
     * ---------------------------------------------------------------- */
    printf("  Key-Rate DV01  (5Y BKBM par swap, per +1bp on each BKBM IRS)\n");
    print_separator();
    printf("  %-4s  %-8s  %-12s\n", "Idx", "Mat (Y)", "DV01 (bp/bp)");
    print_separator();

    double dv01[MAX_NODES];
    computeKeyRateDV01(ibor_instruments, n_ibor, &oisCurve, 5.0, 4, dv01);

    double sum_dv01 = 0.0;
    for (int i = 0; i < n_ibor; i++) {
        printf("  [%2d]  %6.3f   %+12.6f\n",
               i, ibor_instruments[i].maturity, dv01[i] * 1e4);
        sum_dv01 += dv01[i] * 1e4;
    }
    print_separator();
    printf("  Sum (approx parallel DV01): %+.6f bp/bp\n\n", sum_dv01);

    printf("================================================================\n");
    return 0;
}
