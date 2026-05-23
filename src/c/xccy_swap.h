
#ifndef XCCY_SWAP_H
#define XCCY_SWAP_H

#include "dual_curve.h"   /* InterestRateCurve, SwapCashFlow, etc. */

/* =========================================================
 * Cross-Currency (XCCY) Basis Swap Extension
 * ---------------------------------------------------------
 * Supports the standard market convention:
 *   - Domestic  leg : Float (IBOR) + basis spread, quarterly
 *   - Foreign   leg : Float (IBOR), quarterly
 *   - Notional  exchange at start AND end (resettable or fixed)
 *   - Discounting via each currency's OIS curve
 *
 * Terminology used throughout:
 *   "dom"  = domestic currency (USD / pay-fixed side in conventional quotes)
 *   "for"  = foreign currency
 *   FX spot = units of domestic per 1 unit of foreign (e.g. USD/EUR)
 * ========================================================= */

#define MAX_XCCY_PERIODS 240     /* 20-year swap, quarterly = 80; headroom to 60y */

/* ----------------------------------------------------------
 * XCCYLeg
 * Represents one currency side of the basis swap.
 * isFixed=0 always for XCCY (both legs float), but the field
 * is retained so calculateLegPV() from dual_curve.c can be
 * reused directly on either leg.
 * ---------------------------------------------------------- */
typedef struct {
    SwapCashFlow  periods[MAX_XCCY_PERIODS];
    int           numPeriods;
    int           isFixed;        /* 0 = floating (always for XCCY) */
    double        notional;       /* in that leg's own currency      */
} XCCYLeg;

/* ----------------------------------------------------------
 * XCCYSwap – complete trade container
 * ---------------------------------------------------------- */
typedef struct {
    XCCYLeg  domLeg;              /* domestic leg (carries the basis spread) */
    XCCYLeg  forLeg;              /* foreign  leg (flat floating)            */

    double   fxSpot;              /* dom/for spot rate at trade inception    */
    double   maturity;            /* trade maturity in years                 */
    int      resettableNotional;  /* 1 = mark-to-market notional resets      */
} XCCYSwap;

/* ----------------------------------------------------------
 * XCCYResult – outputs from pricing / solving
 * ---------------------------------------------------------- */
typedef struct {
    double npvDomCcy;       /* NPV in domestic currency                        */
    double basisSpreadBps;  /* fair basis spread in basis points               */
    double pvDomLeg;        /* PV of domestic (spread) leg in dom ccy          */
    double pvForLegDom;     /* PV of foreign leg, converted to dom ccy via FX  */
    double dv01Dom;         /* DV01 w.r.t. domestic curve (1bp parallel shift) */
    double dv01For;         /* DV01 w.r.t. foreign  curve (1bp parallel shift) */
    double fxDelta;         /* sensitivity to a 1% move in FX spot             */
} XCCYResult;

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

/**
 * buildXCCYSwap
 * Populate an XCCYSwap from raw parameters.
 *
 * @param swap          Output structure to fill
 * @param maturity      Tenor in years (e.g. 5.0)
 * @param frequency     Payment frequency per year (4 = quarterly)
 * @param domNotional   Notional in domestic currency
 * @param fxSpot        Spot FX rate (dom per for)
 * @param basisSpreadBps Spread on the domestic leg in basis points
 * @param resettable    1 if notional resets each period (MtM XCCY)
 */
void buildXCCYSwap(XCCYSwap *swap,
                   double    maturity,
                   int       frequency,
                   double    domNotional,
                   double    fxSpot,
                   double    basisSpreadBps,
                   int       resettable);

/**
 * priceXCCYSwap
 * Mark-to-market a given XCCYSwap using four curves.
 *
 * @param swap          Fully-specified trade
 * @param domFwdCurve   Domestic IBOR projection curve
 * @param domOisCurve   Domestic OIS discount curve
 * @param forFwdCurve   Foreign  IBOR projection curve
 * @param forOisCurve   Foreign  OIS discount curve
 * @param fxSpotNow     Current FX spot (dom per for)
 * @param result        Output structure
 */
void priceXCCYSwap(XCCYSwap         *swap,
                   InterestRateCurve *domFwdCurve,
                   InterestRateCurve *domOisCurve,
                   InterestRateCurve *forFwdCurve,
                   InterestRateCurve *forOisCurve,
                   double             fxSpotNow,
                   XCCYResult        *result);

/**
 * solveXCCYBasisSpread
 * Find the fair basis spread (bps on the domestic leg) that makes NPV = 0.
 *
 * Returns the fair basis spread in basis points.
 */
double solveXCCYBasisSpread(double            maturity,
                            int               frequency,
                            double            domNotional,
                            double            fxSpot,
                            int               resettable,
                            InterestRateCurve *domFwdCurve,
                            InterestRateCurve *domOisCurve,
                            InterestRateCurve *forFwdCurve,
                            InterestRateCurve *forOisCurve);

/**
 * computeXCCYDV01
 * Bump each curve ±1bp and return finite-difference DV01s.
 */
void computeXCCYDV01(XCCYSwap         *swap,
                     InterestRateCurve *domFwdCurve,
                     InterestRateCurve *domOisCurve,
                     InterestRateCurve *forFwdCurve,
                     InterestRateCurve *forOisCurve,
                     double             fxSpotNow,
                     double            *dv01Dom,
                     double            *dv01For);

#endif /* XCCY_SWAP_H */

