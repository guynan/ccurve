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

#endif /* ASSET_SWAP_H */
