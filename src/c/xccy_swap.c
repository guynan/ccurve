/*
 * xccy_swap.c
 * -----------
 * Cross-Currency (XCCY) Basis Swap pricer, built on top of the
 * existing ccurve dual-curve framework in dual_curve.c / dual_curve.h.
 *
 * Market convention implemented:
 *   Domestic leg : 3M/quarterly IBOR + basis spread (quoted in bps)
 *   Foreign  leg : 3M/quarterly IBOR flat
 *   Notional exchange: initial + final (standard), or MtM reset each period
 *   Discounting via each currency's own OIS curve
 *
 * The fair basis spread is the spread added to the domestic (USD) leg so that
 * NPV(dom leg) + NPV(initial notional exchange) + NPV(final notional exchange)
 *   = NPV(for leg) + NPV(for notional exchanges)  [converted to dom via FX]
 *
 * In practice, USD/EUR XCCY basis is quoted as the spread on the EUR leg;
 * swap the "dom" / "for" labels to match your quoting convention.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------
 * Pull in the existing ccurve structures.
 * dual_curve.c exposes getDiscountFactor() and InterestRateCurve,
 * which are all we need from the core library.
 * ------------------------------------------------------------------ */
#include "xccy_swap.h"

/* getDiscountFactor is declared (const-correct) in interp.h via dual_curve.h */

/* ================================================================
 * Internal helpers
 * ================================================================ */

/* Bump all rates on a curve by delta (used for DV01 calculation) */
static void bumpCurveRates(InterestRateCurve *dst,
						   const InterestRateCurve *src,
						   double delta)
{
	memcpy(dst, src, sizeof(InterestRateCurve));
	for (int i = 0; i < dst->numNodes; i++) {
		dst->rates[i] += delta;
		dst->dfs[i]	= exp(-dst->rates[i] * dst->times[i]);
	}
}

/*
 * pvXCCYLeg
 * ---------
 * Compute the PV of one leg of the XCCY swap.
 *
 * For each coupon period i:
 *   PV_i = notional * (fwd_rate + spread) * accrual_frac * df_ois(t_pay)
 *
 * If resettable=1, the notional for the foreign leg is reset each period
 * to reflect the prevailing FX-adjusted notional (simplified: we use a
 * constant notional here, which is the standard for non-MtM XCCY).
 * A full MtM reset implementation would require forward FX points –
 * see the comments in solveXCCYBasisSpread for how to extend this.
 */
static double pvXCCYLeg(const XCCYLeg	 *leg,
						InterestRateCurve *fwdCurve,
						InterestRateCurve *oisCurve)
{
	double pv = 0.0;
	for (int i = 0; i < leg->numPeriods; i++) {
		const SwapCashFlow *cf = &leg->periods[i];
		double df  = getDiscountFactor(oisCurve, cf->paymentTime);
		double dt  = cf->accrualFraction;

		double fwdRate;
		if (leg->isFixed) {
			fwdRate = cf->fixedRate;
		} else {
			/* IBOR forward rate from the projection curve */
			double dfStart = getDiscountFactor(fwdCurve, cf->startTime);
			double dfEnd   = getDiscountFactor(fwdCurve, cf->endTime);
			if (dfEnd <= 0.0 || dt <= 0.0) continue;
			fwdRate = (dfStart / dfEnd - 1.0) / dt;
		}

		pv += cf->notional * (fwdRate + cf->spread) * dt * df;
	}
	return pv;
}

/*
 * pvNotionalExchanges
 * -------------------
 * Returns the PV of the two notional legs (pay at t=0, receive at t=maturity),
 * discounted on the given OIS curve.
 *
 *   PV = -notional * df(0)   +  notional * df(maturity)
 *	  =  notional * (df(maturity) - 1.0)		[df(0)=1 by definition]
 *
 * Note: the sign convention is from the perspective of the receiver of that
 * notional flow.  The caller combines signs appropriately.
 */
static double pvNotionalExchanges(double			notional,
								  double			maturity,
								  InterestRateCurve *oisCurve)
{
	double dfMat = getDiscountFactor(oisCurve, maturity);
	/* Pay notional at t=0 (PV = -notional), receive at maturity (PV = +notional*dfMat) */
	return notional * (dfMat - 1.0);
}

/* ================================================================
 * Public API implementation
 * ================================================================ */

/* ------------------------------------------------------------------
 * buildXCCYSwap
 * ------------------------------------------------------------------ */
void buildXCCYSwap(XCCYSwap *swap,
				   double	maturity,
				   int	   frequency,
				   double	domNotional,
				   double	fxSpot,
				   double	basisSpreadBps,
				   int	   resettable)
{
	memset(swap, 0, sizeof(XCCYSwap));

	swap->fxSpot			 = fxSpot;
	swap->maturity		   = maturity;
	swap->resettableNotional = resettable;

	double dt		   = 1.0 / (double)frequency;
	int	totalPeriods = (int)round(maturity * frequency);
	double forNotional  = domNotional / fxSpot;   /* e.g. EUR notional */
	double spreadDecimal = basisSpreadBps / 10000.0;

	/* ---- Domestic leg (carries the basis spread) ---- */
	swap->domLeg.isFixed	= 0;
	swap->domLeg.notional   = domNotional;
	swap->domLeg.numPeriods = totalPeriods;

	for (int i = 0; i < totalPeriods; i++) {
		SwapCashFlow *cf = &swap->domLeg.periods[i];
		cf->startTime	  = i * dt;
		cf->endTime		= (i + 1) * dt;
		cf->paymentTime	= cf->endTime;
		cf->accrualFraction = dt;
		cf->fixedRate	  = 0.0;
		cf->spread		 = spreadDecimal;
		cf->notional	   = domNotional;
	}

	/* ---- Foreign leg (flat IBOR, no spread) ---- */
	swap->forLeg.isFixed	= 0;
	swap->forLeg.notional   = forNotional;
	swap->forLeg.numPeriods = totalPeriods;

	for (int i = 0; i < totalPeriods; i++) {
		SwapCashFlow *cf = &swap->forLeg.periods[i];
		cf->startTime	  = i * dt;
		cf->endTime		= (i + 1) * dt;
		cf->paymentTime	= cf->endTime;
		cf->accrualFraction = dt;
		cf->fixedRate	  = 0.0;
		cf->spread		 = 0.0;
		cf->notional	   = forNotional;
	}
}

/* ------------------------------------------------------------------
 * priceXCCYSwap
 *
 * NPV (from domestic receiver's perspective):
 *
 *   NPV_dom = PV(dom coupon leg) + PV(dom notional exchanges)
 *   NPV_for = [PV(for coupon leg) + PV(for notional exchanges)] * fxSpotNow
 *
 *   Total NPV (in dom ccy) = NPV_for_converted - NPV_dom
 *
 * The sign convention:
 *   Positive NPV  => in-the-money for the party receiving the foreign leg
 *					and paying the domestic (USD) + spread leg.
 * ------------------------------------------------------------------ */
void priceXCCYSwap(XCCYSwap		 *swap,
				   InterestRateCurve *domFwdCurve,
				   InterestRateCurve *domOisCurve,
				   InterestRateCurve *forFwdCurve,
				   InterestRateCurve *forOisCurve,
				   double			 fxSpotNow,
				   XCCYResult		*result)
{
	memset(result, 0, sizeof(XCCYResult));

	/* --- Coupon PVs --- */
	double pvDom = pvXCCYLeg(&swap->domLeg, domFwdCurve, domOisCurve);
	double pvFor = pvXCCYLeg(&swap->forLeg, forFwdCurve, forOisCurve);

	/* --- Notional exchange PVs --- */
	double pvDomNotional = pvNotionalExchanges(swap->domLeg.notional,
											   swap->maturity, domOisCurve);
	double pvForNotional = pvNotionalExchanges(swap->forLeg.notional,
											   swap->maturity, forOisCurve);

	/*
	 * Total PV of each side, in domestic currency.
	 * The foreign leg's cash flows are in foreign currency –
	 * we convert using fxSpotNow (dom per for).
	 */
	result->pvDomLeg	 = pvDom + pvDomNotional;
	result->pvForLegDom  = (pvFor + pvForNotional) * fxSpotNow;

	/* NPV = receive foreign, pay domestic */
	result->npvDomCcy	= result->pvForLegDom - result->pvDomLeg;

	/* DV01 will be filled by computeXCCYDV01 if needed */
	result->dv01Dom = 0.0;
	result->dv01For = 0.0;

	/* FX delta: dNPV / d(fxSpot) * 0.01 (1% move) */
	result->fxDelta = 0.01 * (pvFor + pvForNotional);

	/* back-solve the basis spread (in bps) that is already embedded */
	/* (this is just a display of the current spread, not a solve)   */
	result->basisSpreadBps = swap->domLeg.numPeriods > 0
							 ? swap->domLeg.periods[0].spread * 10000.0
							 : 0.0;
}

/* ------------------------------------------------------------------
 * solveXCCYBasisSpread
 *
 * We want to find `b` (bps) such that NPV = 0, i.e.:
 *
 *   PV(for leg, converted) + PV(for notionals, converted)
 *	 = PV(dom IBOR leg + spread b) + PV(dom notionals)
 *
 * Re-arranging (annuity A = sum of OIS DFs * accrual fracs * notional):
 *
 *   b = [ PV(for) + PV(for notionals) - PV(dom IBOR flat) - PV(dom notionals) ]
 *		 / A_dom
 *
 * This is a closed-form solve (linear in `b`) – no iteration required.
 * ------------------------------------------------------------------ */
double solveXCCYBasisSpread(double			maturity,
							int			   frequency,
							double			domNotional,
							double			fxSpot,
							int			   resettable,
							InterestRateCurve *domFwdCurve,
							InterestRateCurve *domOisCurve,
							InterestRateCurve *forFwdCurve,
							InterestRateCurve *forOisCurve)
{
	/* Build the swap with zero basis first */
	XCCYSwap swap;
	buildXCCYSwap(&swap, maturity, frequency, domNotional, fxSpot, 0.0, resettable);

	double dt		   = 1.0 / (double)frequency;
	int	totalPeriods = (int)round(maturity * frequency);
	double forNotional  = domNotional / fxSpot;

	/* --- Domestic IBOR PV (spread = 0) --- */
	double pvDomIbor = pvXCCYLeg(&swap.domLeg, domFwdCurve, domOisCurve);

	/* --- Foreign IBOR PV (in for ccy, then convert) --- */
	double pvForIbor = pvXCCYLeg(&swap.forLeg, forFwdCurve, forOisCurve) * fxSpot;

	/* --- Notional exchange PVs --- */
	double pvDomNotional = pvNotionalExchanges(domNotional, maturity, domOisCurve);
	double pvForNotional = pvNotionalExchanges(forNotional, maturity, forOisCurve) * fxSpot;

	/*
	 * Annuity of the domestic leg:
	 *   A = notional * sum_i [ accrual_i * df_ois_dom(t_pay_i) ]
	 */
	double annuity = 0.0;
	for (int i = 1; i <= totalPeriods; i++) {
		double tPay = i * dt;
		annuity += domNotional * dt * getDiscountFactor(domOisCurve, tPay);
	}

	if (fabs(annuity) < 1e-12) return 0.0;

	/*
	 * Fair spread in decimal:
	 *   b_dec = (PVfor_total - PVdom_ibor_total - PVdom_notional) / annuity
	 * where PVfor_total = pvForIbor + pvForNotional
	 *	   PVdom_ibor_total = pvDomIbor + pvDomNotional
	 */
	double pvForTotal = pvForIbor + pvForNotional;
	double pvDomTotal = pvDomIbor + pvDomNotional;

	double basisDecimal = (pvForTotal - pvDomTotal) / annuity;

	return basisDecimal * 10000.0;   /* convert to basis points */
}

/* ------------------------------------------------------------------
 * computeXCCYDV01
 *
 * Parallel bump of +1bp on each curve, central-difference approximation.
 * ------------------------------------------------------------------ */
void computeXCCYDV01(XCCYSwap		 *swap,
					 InterestRateCurve *domFwdCurve,
					 InterestRateCurve *domOisCurve,
					 InterestRateCurve *forFwdCurve,
					 InterestRateCurve *forOisCurve,
					 double			 fxSpotNow,
					 double			*dv01Dom,
					 double			*dv01For)
{
	const double bp = 0.0001;   /* 1 basis point */

	XCCYResult base, up;

	/* --- Base NPV --- */
	priceXCCYSwap(swap, domFwdCurve, domOisCurve,
						forFwdCurve, forOisCurve,
						fxSpotNow, &base);

	/* --- DV01 w.r.t. domestic curves (bump dom fwd + dom ois together) --- */
	{
		InterestRateCurve domFwdBump, domOisBump;
		bumpCurveRates(&domFwdBump, domFwdCurve, bp);
		bumpCurveRates(&domOisBump, domOisCurve, bp);

		priceXCCYSwap(swap, &domFwdBump, &domOisBump,
							forFwdCurve,  forOisCurve,
							fxSpotNow, &up);

		*dv01Dom = up.npvDomCcy - base.npvDomCcy;
	}

	/* --- DV01 w.r.t. foreign curves (bump for fwd + for ois together) --- */
	{
		InterestRateCurve forFwdBump, forOisBump;
		bumpCurveRates(&forFwdBump, forFwdCurve, bp);
		bumpCurveRates(&forOisBump, forOisCurve, bp);

		priceXCCYSwap(swap,  domFwdCurve,  domOisCurve,
							 &forFwdBump, &forOisBump,
							 fxSpotNow, &up);

		*dv01For = up.npvDomCcy - base.npvDomCcy;
	}
}

/* ================================================================
 * Convenience: print a summary of the pricing result to stdout
 * ================================================================ */
void printXCCYResult(const XCCYResult *result, double maturity)
{
	printf("\n========================================================\n");
	printf("  XCCY Basis Swap Pricing Summary  (tenor = %.1f yr)\n", maturity);
	printf("========================================================\n");
	printf("  NPV (dom ccy)		  : %+14.4f\n", result->npvDomCcy);
	printf("  PV dom leg (dom ccy)   : %+14.4f\n", result->pvDomLeg);
	printf("  PV for leg (dom ccy)   : %+14.4f\n", result->pvForLegDom);
	printf("  Current basis spread   : %+10.2f bps\n", result->basisSpreadBps);
	printf("  DV01 dom curves		: %+14.4f\n", result->dv01Dom);
	printf("  DV01 for curves		: %+14.4f\n", result->dv01For);
	printf("  FX delta (1%% move)	 : %+14.4f\n", result->fxDelta);
	printf("========================================================\n\n");
}

