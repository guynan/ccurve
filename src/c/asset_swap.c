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

/* ================================================================== */
/*  Internal helpers                                                   */
/* ================================================================== */

/* Bond price from a flat continuously-compounded yield y. */
static double bondPriceFromYield(const AssetSwapSpec *spec, double y)
{
    double pv = 0.0;
    for (int i = 0; i < spec->numCoupons; i++) {
        double cf = spec->couponRate * spec->accrualFracs[i];
        pv += cf * exp(-y * spec->couponTimes[i]);
    }
    pv += exp(-y * spec->maturityTime);  /* principal */
    return pv;
}

/* d(bond price)/dy — closed-form derivative w.r.t. yield. */
static double bondPriceDeriv(const AssetSwapSpec *spec, double y)
{
    double dpdy = 0.0;
    for (int i = 0; i < spec->numCoupons; i++) {
        double cf = spec->couponRate * spec->accrualFracs[i];
        dpdy -= spec->couponTimes[i] * cf * exp(-y * spec->couponTimes[i]);
    }
    dpdy -= spec->maturityTime * exp(-y * spec->maturityTime);
    return dpdy;
}

/* ================================================================== */
/*  Bond PV (curve-discounted)                                         */
/* ================================================================== */

double computeBondPV(const AssetSwapSpec     *spec,
                     const InterestRateCurve *discCurve)
{
    if (spec->numCoupons <= 0) return 0.0;

    double pv = 0.0;
    for (int i = 0; i < spec->numCoupons; i++) {
        double cf = spec->couponRate * spec->accrualFracs[i];
        pv += cf * getDiscountFactor(discCurve, spec->couponTimes[i]);
    }
    pv += getDiscountFactor(discCurve, spec->maturityTime);
    return pv;
}

/* ================================================================== */
/*  Yield to maturity (Newton-Raphson)                                 */
/* ================================================================== */

double solveYTM(const AssetSwapSpec *spec)
{
    if (spec->numCoupons <= 0 || spec->dirtyPrice <= 0.0) return 0.0;

    /* Initial guess: approximate flat rate that discounts maturity to price */
    double y = (spec->maturityTime > 1e-9)
               ? -log(spec->dirtyPrice) / spec->maturityTime
               : spec->couponRate;

    for (int iter = 0; iter < 50; iter++) {
        double f  = bondPriceFromYield(spec, y) - spec->dirtyPrice;
        double fp = bondPriceDeriv(spec, y);
        if (fabs(fp) < 1e-15) break;
        double step = f / fp;
        y -= step;
        if (fabs(step) < 1e-10) break;
    }
    return y;
}

/* ================================================================== */
/*  Bond DV01 (analytic, parallel zero-curve shift)                    */
/* ================================================================== */

double computeBondDV01(const AssetSwapSpec     *spec,
                       const InterestRateCurve *discCurve)
{
    if (spec->numCoupons <= 0) return 0.0;

    /* d(PV)/d(z) = -sum_i t_i * CF_i * DF(t_i)
     * DV01 = -d(PV)/d(z) * 1e-4  (positive for long bond, +1bp shift) */
    double dollar_dur = 0.0;
    for (int i = 0; i < spec->numCoupons; i++) {
        double cf = spec->couponRate * spec->accrualFracs[i];
        dollar_dur += spec->couponTimes[i] * cf
                      * getDiscountFactor(discCurve, spec->couponTimes[i]);
    }
    dollar_dur += spec->maturityTime
                  * getDiscountFactor(discCurve, spec->maturityTime);

    return 1e-4 * dollar_dur;
}

/* ================================================================== */
/*  Modified duration                                                   */
/* ================================================================== */

double computeBondModifiedDuration(const AssetSwapSpec     *spec,
                                   const InterestRateCurve *discCurve)
{
    if (spec->dirtyPrice <= 0.0) return 0.0;
    return computeBondDV01(spec, discCurve) / (1e-4 * spec->dirtyPrice);
}

/* ================================================================== */
/*  Convexity (yield-based, closed-form)                               */
/* ================================================================== */

double computeBondConvexity(const AssetSwapSpec *spec)
{
    if (spec->numCoupons <= 0 || spec->dirtyPrice <= 0.0) return 0.0;

    double ytm = solveYTM(spec);

    /* convexity = (1/P) * sum_i t_i^2 * CF_i * exp(-ytm * t_i) */
    double convex = 0.0;
    for (int i = 0; i < spec->numCoupons; i++) {
        double cf = spec->couponRate * spec->accrualFracs[i];
        double t  = spec->couponTimes[i];
        convex += t * t * cf * exp(-ytm * t);
    }
    convex += spec->maturityTime * spec->maturityTime
              * exp(-ytm * spec->maturityTime);

    return convex / spec->dirtyPrice;
}

/* ================================================================== */
/*  Z-spread (Newton-Raphson)                                          */
/* ================================================================== */

double solveZSpread(const AssetSwapSpec     *spec,
                    const InterestRateCurve *discCurve)
{
    if (spec->numCoupons <= 0 || spec->dirtyPrice <= 0.0) return 0.0;

    double z = 0.0;  /* start at zero spread */

    for (int iter = 0; iter < 50; iter++) {
        double f  = 0.0;  /* PV(z) - dirtyPrice */
        double fp = 0.0;  /* d(PV)/dz            */

        for (int i = 0; i < spec->numCoupons; i++) {
            double t    = spec->couponTimes[i];
            double cf   = spec->couponRate * spec->accrualFracs[i];
            double disc = getDiscountFactor(discCurve, t) * exp(-z * t);
            f  += cf * disc;
            fp -= t  * cf * disc;
        }
        double tMat    = spec->maturityTime;
        double discMat = getDiscountFactor(discCurve, tMat) * exp(-z * tMat);
        f  += discMat;
        fp -= tMat * discMat;

        f -= spec->dirtyPrice;

        if (fabs(fp) < 1e-15) break;
        double step = f / fp;
        z -= step;
        if (fabs(step) < 1e-10) break;
    }
    return z;
}

/* ================================================================== */
/*  Matched-maturity swap rate                                          */
/* ================================================================== */

double solveMatchedMaturitySwapRate(const AssetSwapSpec     *spec,
                                    const InterestRateCurve *fwdCurve,
                                    const InterestRateCurve *discCurve)
{
    return solveParSwapRate(fwdCurve, discCurve,
                            spec->maturityTime,
                            (int32_t)spec->couponFrequency);
}

/* ================================================================== */
/*  Asset swap upfront fee                                             */
/* ================================================================== */

double computeAssetSwapUpfrontFee(const AssetSwapSpec     *spec,
                                  const InterestRateCurve *discCurve)
{
    /* Market-value ASW: investor pays dirtyPrice, swap notional is par (1.0).
     * The upfront fee is the difference between the bond's curve PV and par,
     * i.e. how far the bond trades from fair par value on the swap curve. */
    return computeBondPV(spec, discCurve) - 1.0;
}
