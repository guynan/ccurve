#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dual_curve.h"
#include "asset_swap.h"

/* ================================================================== */
/*  Bond schedule builder                                              */
/* ================================================================== */

int buildBondSchedule(AssetSwapSpec *spec,
                      double firstCouponTime,
                      double maturityTime,
                      int    frequency,
                      double couponRate,
                      DayCountFraction dcf)
{
    if (frequency <= 0 || maturityTime <= firstCouponTime) return -1;

    spec->couponRate      = couponRate;
    spec->couponFrequency = frequency;
    spec->couponDcf       = dcf;
    spec->maturityTime    = maturityTime;

    double dt         = 1.0 / (double)frequency;
    int    nMaxPeriods = MAX_BOND_COUPONS;

    int n = 0;
    double tPrev = firstCouponTime - dt;   /* approximate period-start before first coupon */
    double tCpn  = firstCouponTime;

    while (tCpn <= maturityTime + 1e-9 && n < nMaxPeriods) {
        spec->couponTimes[n]  = tCpn;
        spec->accrualFracs[n] = tCpn - tPrev;   /* Act/365 approx; exact dcf via yearFraction */
        tPrev = tCpn;
        tCpn += dt;
        n++;
    }

    spec->numCoupons = n;
    return n;
}

/* ================================================================== */
/*  Par asset swap spread (closed-form)                                */
/* ================================================================== */

double solveAssetSwapSpread(const AssetSwapSpec     *spec,
                            const InterestRateCurve *projCurve,
                            const InterestRateCurve *discCurve)
{
    (void)projCurve;   /* not needed for par-ASW: spread is discount-only */

    if (spec->numCoupons <= 0) return 0.0;

    /* PV of bond cash flows discounted at discCurve.
     *
     * Par-ASW formula:
     *   PV_bond = sum_i( coupon_rate/freq * alpha_i * DF_disc(tCpn_i) )
     *             + DF_disc(maturity) - dirtyPrice
     *
     * where coupon_rate/freq is the per-period coupon amount (per unit face).
     * alpha_i is the day-count adjusted accrual fraction.
     *
     * The annuity (floating-leg PV per unit spread) is:
     *   annuity = sum_i( alpha_i * DF_disc(tPay_i) )
     *
     * For the floating leg, tPay_i carries any payment lag from spec->floatLeg.
     * When paymentLagDays == 0, tPay_i = tCpn_i (standard behaviour). */
    double pvBond  = 0.0;
    double annuity = 0.0;

    for (int i = 0; i < spec->numCoupons; i++) {
        double tCpn  = spec->couponTimes[i];
        double alpha = spec->accrualFracs[i];
        double dfDisc = getDiscountFactor(discCurve, tCpn);

        /* Bond coupon PV: annual coupon rate × accrual fraction × DF */
        pvBond += spec->couponRate * alpha * dfDisc;

        /* Floating leg annuity: payment lag shifts tPay for the swap leg */
        double tPay = tCpn + spec->floatLeg.paymentLagDays / 260.0;
        double dfPay = getDiscountFactor(discCurve, tPay);
        annuity += alpha * dfPay;
    }

    /* Add principal redemption at maturity */
    double dfMat = getDiscountFactor(discCurve, spec->maturityTime);
    pvBond += dfMat;

    /* Net bond PV relative to dirty price (investor pays dirtyPrice at t=0) */
    pvBond -= spec->dirtyPrice;

    if (fabs(annuity) < 1e-12) return 0.0;
    return pvBond / annuity;
}

/* ================================================================== */
/*  Asset swap DV01 (parallel bump-and-reprice)                       */
/* ================================================================== */

double computeAssetSwapDV01(const AssetSwapSpec     *spec,
                            const MarketInstrument  *fwdInstruments,
                            int32_t                  numFwdInstruments,
                            const InterestRateCurve *discCurve)
{
    /* Bootstrap a base forward curve and compute base spread */
    InterestRateCurve fwdBase;
    memset(&fwdBase, 0, sizeof(fwdBase));
    bootstrapCurve(&fwdBase, discCurve, fwdInstruments, numFwdInstruments, NULL);
    double sBase = solveAssetSwapSpread(spec, &fwdBase, discCurve);

    /* Bump all forward instruments by +1bp and rebootstrap */
    MarketInstrument bumped[MAX_NODES];
    for (int32_t i = 0; i < numFwdInstruments; i++) {
        bumped[i] = fwdInstruments[i];
        if (fwdInstruments[i].type == FUTURE)
            bumped[i].price -= 0.01;
        else
            bumped[i].rate  += 1e-4;
    }
    InterestRateCurve fwdBump;
    memset(&fwdBump, 0, sizeof(fwdBump));
    bootstrapCurve(&fwdBump, discCurve, bumped, numFwdInstruments, NULL);
    double sBump = solveAssetSwapSpread(spec, &fwdBump, discCurve);

    return sBump - sBase;  /* change in spread per +1bp parallel shift */
}
