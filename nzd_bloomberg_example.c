/*
 * nzd_bloomberg_example.c
 *
 * NZD dual-curve bootstrap via Bloomberg.
 *
 * Two independent curve legs fetched in a single BDP call:
 *
 *   NZONIA OIS (discount curve)
 *     NDSO{1..6} Curncy   — 1Y-6Y NZONIA OIS swaps
 *
 *   BKBM forward curve (projection curve)
 *     NDBB3M Curncy       — 3M BKBM bank bill deposit
 *     ZB1–ZB4 Comdty      — ASX 90-day bank bill futures
 *     NDSWAP{3,4,5,6,7,10,12,15} Curncy — quarterly BKBM IRS
 *
 * The two curves are built independently and then combined:
 *   bootstrapOisCurve   → NZONIA OIS (self-discounted)
 *   bootstrapCurve      → BKBM forward, discounted by NZONIA OIS
 *
 * Build:
 *   make bloomberg && make nzd_example
 *
 * Run:
 *   ./nzd_example [YYYY-MM-DD] [blp-host] [blp-port]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dual_curve.h"
#include "blpapi_fetcher.h"

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

static void sep(void)
{
    printf("  %s\n", "------------------------------------------------------------");
}

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
    printf("  OIS: NZONIA (NDSO1-6 year-tenor swaps)\n");
    printf("  FWD: BKBM   (NDBB3M + ZB futures + NDSWAP IRS)\n");
    printf("================================================================\n");
    printf("  Anchor date : %s\n", as_of_date);
    printf("  Bloomberg   : %s:%d\n\n", blp_host, blp_port);

    /* ----------------------------------------------------------------
     * 1. Connect
     * ---------------------------------------------------------------- */
    BlpSession *session = blp_session_create(blp_host, blp_port);
    if (!blp_session_connected(session)) {
        fprintf(stderr,
            "Error: cannot connect to Bloomberg at %s:%d.\n", blp_host, blp_port);
        blp_session_destroy(session);
        return 1;
    }

    /* ----------------------------------------------------------------
     * 2. Fetch all 19 instruments in one BDP call
     * ---------------------------------------------------------------- */
    MarketInstrument all_inst[MAX_NODES];
    memset(all_inst, 0, sizeof(all_inst));

    int n_all = blp_fetch_nzd_curve_instruments(
                    session, all_inst, MAX_NODES, as_of_date);
    blp_session_destroy(session);

    if (n_all <= 0) {
        fprintf(stderr, "Error: fetch returned %d instruments.\n", n_all);
        return 1;
    }

    /* Split: OIS_SWAP → ois_inst[], everything else → bkbm_inst[] */
    MarketInstrument ois_inst[MAX_NODES];
    MarketInstrument bkbm_inst[MAX_NODES];
    int n_ois = 0, n_bkbm = 0;

    for (int i = 0; i < n_all; i++) {
        if (all_inst[i].type == OIS_SWAP)
            ois_inst[n_ois++]   = all_inst[i];
        else
            bkbm_inst[n_bkbm++] = all_inst[i];
    }

    printf("  Fetched %d instruments  (%d OIS, %d BKBM):\n\n",
           n_all, n_ois, n_bkbm);
    printf("  %-4s  %-8s  %-8s  %-10s\n", "Idx", "Type", "Mat (Y)", "Rate / Price");
    sep();
    for (int i = 0; i < n_all; i++) {
        const MarketInstrument *ins = &all_inst[i];
        if (ins->type == FUTURE)
            printf("  [%2d]  %-7s  %6.3f   price=%.4f\n",
                   i, inst_type_name(ins->type), ins->maturity, ins->price);
        else
            printf("  [%2d]  %-7s  %6.3f   rate=%.4f%%\n",
                   i, inst_type_name(ins->type), ins->maturity, ins->rate * 100.0);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 3. Bootstrap NZONIA OIS discount curve  (self-discounted)
     * ---------------------------------------------------------------- */
    InterestRateCurve oisCurve;
    memset(&oisCurve, 0, sizeof(oisCurve));

    printf("  Bootstrapping NZONIA OIS curve (%d meeting swaps) ...\n", n_ois);
    if (bootstrapOisCurve(&oisCurve, ois_inst, n_ois, NULL) != 0) {
        fprintf(stderr, "Error: bootstrapOisCurve failed.\n");
        return 1;
    }
    printf("  Done. %d nodes.\n\n", oisCurve.numNodes);

    /* ----------------------------------------------------------------
     * 4. Bootstrap BKBM forward curve  (discounted by OIS)
     * ---------------------------------------------------------------- */
    InterestRateCurve bkbmCurve;
    memset(&bkbmCurve, 0, sizeof(bkbmCurve));

    printf("  Bootstrapping BKBM forward curve (%d instruments) ...\n", n_bkbm);
    if (bootstrapCurve(&bkbmCurve, &oisCurve, bkbm_inst, n_bkbm, NULL) != 0) {
        fprintf(stderr, "Error: bootstrapCurve (BKBM) failed.\n");
        return 1;
    }
    printf("  Done. %d nodes.\n\n", bkbmCurve.numNodes);

    /* ----------------------------------------------------------------
     * 5. NZONIA OIS zero curve
     * ---------------------------------------------------------------- */
    printf("  NZONIA OIS Zero Curve (continuous, Act/365)\n");
    sep();
    printf("  %-4s  %-8s  %-12s  %-12s\n",
           "Idx", "Mat (Y)", "Zero Rate %", "Disc Factor");
    sep();
    for (int i = 0; i < oisCurve.numNodes; i++)
        printf("  [%2d]  %6.3f   %10.4f%%   %.8f\n",
               i, oisCurve.times[i],
               oisCurve.rates[i] * 100.0, oisCurve.dfs[i]);
    printf("\n");

    /* ----------------------------------------------------------------
     * 6. BKBM forward zero curve
     * ---------------------------------------------------------------- */
    printf("  BKBM Forward Zero Curve (continuous, Act/365)\n");
    sep();
    printf("  %-4s  %-8s  %-12s  %-12s\n",
           "Idx", "Mat (Y)", "Zero Rate %", "Disc Factor");
    sep();
    for (int i = 0; i < bkbmCurve.numNodes; i++)
        printf("  [%2d]  %6.3f   %10.4f%%   %.8f\n",
               i, bkbmCurve.times[i],
               bkbmCurve.rates[i] * 100.0, bkbmCurve.dfs[i]);
    printf("\n");

    /* ----------------------------------------------------------------
     * 7. OIS-BKBM basis spread
     * ---------------------------------------------------------------- */
    BasisCurve basis;
    memset(&basis, 0, sizeof(basis));
    computeBasisCurve(&bkbmCurve, &oisCurve, &basis);

    printf("  OIS-BKBM Basis Spread (BKBM zero minus NZONIA zero)\n");
    sep();
    printf("  %-8s  %-12s\n", "Mat (Y)", "Spread (bps)");
    sep();
    for (int i = 0; i < basis.numNodes; i++)
        printf("  %6.3f   %+10.4f\n", basis.times[i], basis.spreadsBps[i]);
    printf("\n");

    /* ----------------------------------------------------------------
     * 8. Par BKBM swap rate verification  (residuals should be < 0.01 bp)
     * ---------------------------------------------------------------- */
    static const double SWAP_TENORS[] = { 3.0, 4.0, 5.0, 6.0,
                                           7.0, 10.0, 12.0, 15.0 };
    int nTenors = (int)(sizeof(SWAP_TENORS) / sizeof(SWAP_TENORS[0]));

    printf("  Par BKBM Swap Rate Verification (quarterly, Act/365)\n");
    sep();
    printf("  %-8s  %-12s  %-12s  %-10s\n",
           "Tenor", "Par Rate %", "Market %", "Error (bp)");
    sep();
    for (int ti = 0; ti < nTenors; ti++) {
        double par = solveParSwapRate(&bkbmCurve, &oisCurve,
                                      SWAP_TENORS[ti], 4);
        double mkt = 0.0;
        for (int j = 0; j < n_bkbm; j++) {
            if (bkbm_inst[j].type == SWAP &&
                fabs(bkbm_inst[j].maturity - SWAP_TENORS[ti]) < 0.01) {
                mkt = bkbm_inst[j].rate;
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
    sep();
    printf("  %-12s  %-12s  %-12s\n", "Start", "End", "Fwd rate %");
    sep();

    static const struct { double s; double e; } FWD_PAIRS[] = {
        { 0.00, 0.25 }, { 0.25, 0.50 }, { 0.50, 0.75 },
        { 0.75, 1.00 }, { 1.00, 1.25 }, { 1.25, 2.00 },
        { 2.00, 3.00 }, { 3.00, 5.00 }, { 5.00, 7.00 },
        { 7.00, 10.00 }, { 10.00, 15.00 },
    };
    int nFwd = (int)(sizeof(FWD_PAIRS) / sizeof(FWD_PAIRS[0]));
    for (int i = 0; i < nFwd; i++) {
        double fwd = getForwardRate(&bkbmCurve, FWD_PAIRS[i].s, FWD_PAIRS[i].e);
        printf("  %7.2fY      %7.2fY      %10.4f%%\n",
               FWD_PAIRS[i].s, FWD_PAIRS[i].e, fwd * 100.0);
    }
    printf("\n");

    /* ----------------------------------------------------------------
     * 10. Key-rate DV01 for 5Y BKBM par swap (wrt BKBM instruments)
     * ---------------------------------------------------------------- */
    printf("  Key-Rate DV01  (5Y BKBM par swap, per +1bp on each BKBM input)\n");
    sep();
    printf("  %-4s  %-8s  %-8s  %-12s\n",
           "Idx", "Type", "Mat (Y)", "DV01 (bp/bp)");
    sep();

    double dv01[MAX_NODES];
    computeKeyRateDV01(bkbm_inst, n_bkbm, &oisCurve, 5.0, 4, dv01);

    double sum_dv01 = 0.0;
    for (int i = 0; i < n_bkbm; i++) {
        printf("  [%2d]  %-7s  %6.3f   %+12.6f\n",
               i, inst_type_name(bkbm_inst[i].type),
               bkbm_inst[i].maturity, dv01[i] * 1e4);
        sum_dv01 += dv01[i] * 1e4;
    }
    sep();
    printf("  Sum (approx parallel DV01): %+.6f bp/bp\n\n", sum_dv01);

    printf("================================================================\n");
    return 0;
}
