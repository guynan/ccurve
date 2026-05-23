#ifndef ASSET_SWAP_H
#define ASSET_SWAP_H

#include "dual_curve.h"

#define MAX_BOND_COUPONS 62  /* covers 30Y semi-annual */

/* Full specification of an asset swap (bond + pay-fixed, receive-floating swap).
 * The par-ASW spread is the spread over the floating index that makes the
 * combined position worth zero from the floating-rate receiver's perspective. */
typedef struct {
    double couponRate;                       /* annual coupon as decimal, e.g. 0.05 */
    double dirtyPrice;                       /* fraction of face, e.g. 1.02 */
    int    couponFrequency;                  /* coupons per year: 2 = semi-annual */
    int    numCoupons;
    double couponTimes[MAX_BOND_COUPONS];    /* year fractions of each coupon date */
    double accrualFracs[MAX_BOND_COUPONS];   /* day-count adjusted coupon fraction */
    double maturityTime;                     /* final coupon / principal redemption */
    DayCountFraction  couponDcf;             /* day count for coupon accruals */
    FloatingRateIndex floatLeg;              /* floating leg conventions */
} AssetSwapSpec;

/* Build a regular coupon schedule from first coupon time to maturity.
 * Fills spec->couponTimes, spec->accrualFracs, spec->numCoupons, spec->maturityTime.
 * Returns number of coupons, or -1 on error. */
int buildBondSchedule(AssetSwapSpec *spec,
                      double firstCouponTime,
                      double maturityTime,
                      int    frequency,
                      double couponRate,
                      DayCountFraction dcf);

/* Par asset swap spread: the fixed spread over the floating index such that
 *
 *   PV_bond(discCurve) + PV_floatLeg(projCurve, discCurve, spread) = 0
 *
 * Closed-form solution (no iteration required):
 *
 *   PV_bond  = sum_i( coupon_i * alpha_i * DF_disc(tCpn_i) ) + DF_disc(T) - dirtyPrice
 *   annuity  = sum_i( alpha_i * DF_disc(tPay_i) )
 *   spread   = PV_bond / annuity
 *
 * Returns spread in decimal (multiply by 10000 for bps).
 * projCurve is used to project the floating index; discCurve discounts all cash flows. */
double solveAssetSwapSpread(const AssetSwapSpec     *spec,
                            const InterestRateCurve *projCurve,
                            const InterestRateCurve *discCurve);

/* Parallel DV01 of the asset swap spread: bump all projCurve calibration
 * instruments by +1bp, rebootstrap, recompute spread.
 * Returns change in spread per +1bp parallel shift (in decimal). */
double computeAssetSwapDV01(const AssetSwapSpec     *spec,
                            const MarketInstrument  *fwdInstruments,
                            int32_t                  numFwdInstruments,
                            const InterestRateCurve *discCurve);

/* ── Extended bond analytics ─────────────────────────────────────────── */

/* PV of all bond cash flows (coupons + principal) discounted at discCurve.
 * Does not subtract dirtyPrice — returns the raw present value. */
double computeBondPV(const AssetSwapSpec     *spec,
                     const InterestRateCurve *discCurve);

/* Yield to maturity (continuously-compounded) implied by spec->dirtyPrice.
 * Solved via Newton-Raphson on the bond pricing equation.
 * Returns YTM in decimal (e.g. 0.052 for 5.20%). */
double solveYTM(const AssetSwapSpec *spec);

/* Dollar DV01: analytic sensitivity of bond PV to a +1bp parallel shift
 * of the zero curve.  Exact closed-form (no re-bootstrap):
 *   DV01 = 1e-4 * [ sum_i( t_i * couponRate * alpha_i * DF(t_i) )
 *                   + maturityTime * DF(maturityTime) ]
 * Returns positive value for a long bond position (per unit face). */
double computeBondDV01(const AssetSwapSpec     *spec,
                       const InterestRateCurve *discCurve);

/* Modified duration: DV01 / dirtyPrice (years).
 * Equivalent to the weighted-average time of discounted cash flows. */
double computeBondModifiedDuration(const AssetSwapSpec     *spec,
                                   const InterestRateCurve *discCurve);

/* Yield-to-maturity convexity: (1/dirtyPrice) * d²P/dy² where y is the YTM.
 * Closed-form: convexity = (1/P) * sum_i( t_i^2 * CF_i * exp(-ytm * t_i) )
 * Measures curvature of the price-yield relationship (years²). */
double computeBondConvexity(const AssetSwapSpec *spec);

/* Z-spread: constant spread z (in decimal) over discCurve zero rates that
 * reprices the bond to its dirty price:
 *   dirtyPrice = sum_i( CF_i * DF_disc(t_i) * exp(-z * t_i) ) + DF_disc(T)*exp(-z*T)
 * Solved via Newton-Raphson.  Returns z in decimal (multiply by 1e4 for bps). */
double solveZSpread(const AssetSwapSpec     *spec,
                    const InterestRateCurve *discCurve);

/* Matched-maturity par swap rate: par swap rate at the bond's maturity and
 * coupon frequency under dual-curve discounting.  Thin wrapper around
 * solveParSwapRate(fwdCurve, discCurve, spec->maturityTime, spec->couponFrequency). */
double solveMatchedMaturitySwapRate(const AssetSwapSpec     *spec,
                                    const InterestRateCurve *fwdCurve,
                                    const InterestRateCurve *discCurve);

/* Asset swap upfront fee (market-value ASW convention):
 *   upfront = computeBondPV(spec, discCurve) - 1.0
 * A positive value means the bond trades above par; the investor
 * pays this premium upfront.  Returns fee per unit face. */
double computeAssetSwapUpfrontFee(const AssetSwapSpec     *spec,
                                  const InterestRateCurve *discCurve);

#endif /* ASSET_SWAP_H */
