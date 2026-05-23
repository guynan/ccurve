#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include "date_utils.h"
#include "dual_curve.h"
#include "file_utils.h"

/* ================================================================== */
/*  Par swap rate solver                                               */
/* ================================================================== */

double solveParSwapRate(const InterestRateCurve *fwdCurve,
                        const InterestRateCurve *oisCurve,
                        double maturity, int32_t frequency)
{
    double dt_approx = 1.0 / frequency;
    int32_t payments  = (int)(maturity * frequency + 0.5);
    double floatLeg   = 0.0, annuity = 0.0;
    double tPrev      = 0.0;

    for (int32_t i = 1; i <= payments; i++) {
        double tPay = i * dt_approx;
        /* Accrual fraction: year fraction of the actual period.
         * Using actual elapsed time rather than the fixed 1/freq captures
         * any day-count irregularity introduced by calendar rolling. */
        double dt       = tPay - tPrev;
        double dfFwdPay = getDiscountFactor(fwdCurve, tPay);
        double dfFwdPrv = getDiscountFactor(fwdCurve, tPrev);
        double fwdRate  = (dt > 0.0) ? (dfFwdPrv / dfFwdPay - 1.0) / dt : 0.0;
        double dfOis    = getDiscountFactor(oisCurve, tPay);
        floatLeg += fwdRate * dt * dfOis;
        annuity  += dt * dfOis;
        tPrev = tPay;
    }
    return (annuity > 0.0) ? floatLeg / annuity : 0.0;
}

double solveForwardParSwapRate(const InterestRateCurve *fwdCurve,
                               const InterestRateCurve *oisCurve,
                               double forwardStart, double swapTenor,
                               int32_t frequency)
{
    double dt          = 1.0 / (double)frequency;
    int32_t totalPeriods = (int)round(swapTenor * frequency);
    double floatPV = 0.0, annuity = 0.0;

    for (int32_t p = 1; p <= totalPeriods; p++) {
        double tPay   = forwardStart + p * dt;
        double tStart = tPay - dt;
        double dfOis  = getDiscountFactor(oisCurve, tPay);
        double dfFwdS = getDiscountFactor(fwdCurve, tStart);
        double dfFwdE = getDiscountFactor(fwdCurve, tPay);
        double impliedFwd = (dfFwdS / dfFwdE - 1.0) / dt;
        floatPV += impliedFwd * dt * dfOis;
        annuity += dt * dfOis;
    }
    return (annuity > 0.0) ? floatPV / annuity : 0.0;
}

/* ================================================================== */
/*  Bootstrap engine                                                   */
/* ================================================================== */

int bootstrapCurve(InterestRateCurve            *fwdCurve,
                   const InterestRateCurve       *oisCurve,
                   const MarketInstrument        *instruments,
                   int32_t                        numInstruments,
                   const CurveConstructionParams *params)
{
    /* Anchor node: t=0, DF=1, rate = first instrument's rate */
    fwdCurve->numNodes  = 0;
    fwdCurve->times[0]  = 0.0;
    fwdCurve->rates[0]  = instruments[0].rate;
    fwdCurve->dfs[0]    = 1.0;
    fwdCurve->numNodes  = 1;

    for (int i = 0; i < numInstruments; i++) {
        const MarketInstrument *inst = &instruments[i];
        int idx = fwdCurve->numNodes;

        if (fwdCurve->numNodes >= MAX_NODES) {
            fprintf(stderr, "bootstrapCurve: MAX_NODES (%d) exceeded\n", MAX_NODES);
            return -1;
        }

        /* ---------------------------------------------------------- */
        if (inst->type == DEPOSIT) {
            double t     = inst->maturity;
            double delta = t - inst->startTime;
            double df    = 1.0 / (1.0 + inst->rate * delta);

            fwdCurve->times[idx] = t;
            fwdCurve->dfs[idx]   = df;
            fwdCurve->rates[idx] = (t > 0.0) ? (-log(df) / t) : inst->rate;
            fwdCurve->numNodes++;
            /* setupMonotoneConvex rebuilds the incremental spline used
             * by getDiscountFactor during mid-bootstrap node lookups.
             * This is separate from the final post-bootstrap regime
             * assignment controlled by CurveConstructionParams. */
            setupMonotoneConvex(fwdCurve);
        }

        /* ---------------------------------------------------------- */
        else if (inst->type == FUTURE) {
            double t_start        = inst->startTime;
            double t_end          = inst->maturity;
            double delta_t        = t_end - t_start;
            double impliedFwdRate = (100.0 - inst->price) / 100.0;

            /* Convexity adjustment: 0.5 * sigma^2 * t_start * t_end
             * Applied when params->convexity.sigma > 0. */
            if (params != NULL && params->convexity.sigma > 0.0) {
                double s = params->convexity.sigma;
                impliedFwdRate -= 0.5 * s * s * t_start * t_end;
            }

            double df_start = getDiscountFactor(fwdCurve, t_start);
            double df_end   = df_start / (1.0 + impliedFwdRate * delta_t);

            fwdCurve->times[idx] = t_end;
            fwdCurve->dfs[idx]   = df_end;
            fwdCurve->rates[idx] = (t_end > 0.0) ? (-log(df_end) / t_end) : 0.0;
            fwdCurve->numNodes++;
            setupMonotoneConvex(fwdCurve);
        }

        /* ---------------------------------------------------------- */
        else if (inst->type == SWAP || inst->type == OIS_SWAP) {
            double t_maturity   = inst->maturity;
            double swapRate     = inst->rate;
            int    freq         = inst->paymentFrequency;
            int    totalPeriods = (int)round(t_maturity * freq);
            double dt           = 1.0 / (double)freq;

            /* For OIS_SWAP the discount curve equals the forward curve
             * (self-discounted single-curve bootstrap). */
            const InterestRateCurve *discCurve =
                (inst->type == OIS_SWAP) ? fwdCurve : oisCurve;

#define EVAL_SWAP_NPV(trial_rate_, npv_out_)                              \
            do {                                                           \
                fwdCurve->times[idx] = t_maturity;                        \
                fwdCurve->rates[idx] = (trial_rate_);                     \
                fwdCurve->dfs[idx]   = exp(-(trial_rate_) * t_maturity);  \
                fwdCurve->numNodes   = idx + 1;                           \
                setupLogDfCubicSpline(fwdCurve);                          \
                double _pvFloat = 0.0, _pvFixed = 0.0, _tPrev = 0.0;     \
                for (int _p = 1; _p <= totalPeriods; _p++) {              \
                    double _tPay   = _p * dt;                             \
                    double _dfDisc = getDiscountFactor(discCurve, _tPay); \
                    double _dfFwdS = getDiscountFactor(fwdCurve, _tPrev); \
                    double _dfFwdE = getDiscountFactor(fwdCurve, _tPay);  \
                    double _fwd    = (_dfFwdS / _dfFwdE - 1.0) / dt;     \
                    _pvFloat += _fwd      * dt * _dfDisc;                 \
                    _pvFixed += swapRate  * dt * _dfDisc;                 \
                    _tPrev = _tPay;                                        \
                }                                                          \
                (npv_out_) = _pvFloat - _pvFixed;                         \
            } while (0)

            double z = swapRate;
            if (z < -0.02) z = -0.02;
            if (z >  0.20) z =  0.20;

            double f = 0.0, f_pert = 0.0, fprime, step;
            int    converged = 0;

            for (int iter = 0; iter < NR_MAX_ITER; iter++) {
                EVAL_SWAP_NPV(z, f);
                if (fabs(f) < NR_TOLERANCE) { converged = 1; break; }

                EVAL_SWAP_NPV(z + NR_DERIV_EPS, f_pert);
                fprime = (f_pert - f) / NR_DERIV_EPS;
                if (fabs(fprime) < 1e-15) break;

                step = f / fprime;
                if (step >  0.005) step =  0.005;
                if (step < -0.005) step = -0.005;
                z -= step;
            }

            if (!converged) {
                double lo = -0.05, hi = 0.50;
                double f_lo, f_hi, f_mid = 0.0, mid = 0.0;
                EVAL_SWAP_NPV(lo, f_lo);
                EVAL_SWAP_NPV(hi, f_hi);
                int expand = 0;
                while (f_lo * f_hi > 0.0 && expand < 20) {
                    lo -= 0.01; hi += 0.01;
                    EVAL_SWAP_NPV(lo, f_lo);
                    EVAL_SWAP_NPV(hi, f_hi);
                    expand++;
                }
                for (int bis = 0; bis < 100; bis++) {
                    mid = 0.5 * (lo + hi);
                    EVAL_SWAP_NPV(mid, f_mid);
                    if (fabs(f_mid) < NR_TOLERANCE) break;
                    if (f_lo * f_mid < 0.0) hi = mid;
                    else { lo = mid; f_lo = f_mid; }
                }
                z = mid;
            }

#undef EVAL_SWAP_NPV

            fwdCurve->times[idx] = t_maturity;
            fwdCurve->rates[idx] = z;
            fwdCurve->dfs[idx]   = exp(-z * t_maturity);
            fwdCurve->numNodes   = idx + 1;
            setupMonotoneConvex(fwdCurve);
        }
    }

    /* ------------------------------------------------------------------
     * Register interpolation regimes on the completed curve.
     *
     * When the caller supplies params->numRegimes > 0, those regimes are
     * used verbatim.  Otherwise the built-in three-regime layout applies:
     *
     *   Regime 0: stepwise OIS  [0 → last CB meeting]     (if schedule set)
     *   Regime 1: log-linear DF [CB boundary → 2Y]
     *   Regime 2: log-DF cubic  [2Y → ∞]
     * ------------------------------------------------------------------ */
    if (params != NULL && params->numRegimes > 0) {
        for (int32_t i = 0; i < params->numRegimes; i++)
            fwdCurve->regimes[i] = params->regimes[i];
        fwdCurve->numRegimes = params->numRegimes;
    } else {
        fwdCurve->numRegimes = 0;

        double cb_boundary = 0.0;
        if (fwdCurve->cbSchedule.numMeetings > 0)
            cb_boundary = fwdCurve->cbSchedule.meetingTimes[
                              fwdCurve->cbSchedule.numMeetings - 1];

        if (cb_boundary > 0.0) {
            fwdCurve->regimes[0].upper_time_boundary = cb_boundary;
            fwdCurve->regimes[0].interp_func         = interpolateStepWiseDF;
            fwdCurve->numRegimes = 1;
        }

        /* Ensure regime 1 boundary is above regime 0 */
        double regime1_boundary = (cb_boundary > LOGDF_REGIME_BOUNDARY)
                                  ? cb_boundary : LOGDF_REGIME_BOUNDARY;

        fwdCurve->regimes[fwdCurve->numRegimes].upper_time_boundary = regime1_boundary;
        fwdCurve->regimes[fwdCurve->numRegimes].interp_func         = interpolateLogLinearDf;
        fwdCurve->numRegimes++;

        fwdCurve->regimes[fwdCurve->numRegimes].upper_time_boundary = 1e9;
        fwdCurve->regimes[fwdCurve->numRegimes].interp_func         = interpolateLogDfCubic;
        fwdCurve->numRegimes++;
    }

    return 0;
}

int bootstrapOisCurve(InterestRateCurve            *oisCurve,
                      const MarketInstrument        *instruments,
                      int32_t                        numInstruments,
                      const CurveConstructionParams *params)
{
    return bootstrapCurve(oisCurve, oisCurve, instruments, numInstruments, params);
}

/* ================================================================== */
/*  Convenience CurveConstructionParams factories                      */
/* ================================================================== */

CurveConstructionParams curveParamsDefault(void)
{
    CurveConstructionParams p;
    memset(&p, 0, sizeof(p));
    /* numRegimes = 0 → bootstrapCurve uses built-in three-regime layout */
    return p;
}

CurveConstructionParams curveParamsLogLinearOnly(void)
{
    CurveConstructionParams p;
    memset(&p, 0, sizeof(p));
    p.numRegimes                        = 1;
    p.regimes[0].upper_time_boundary    = 1e9;
    p.regimes[0].interp_func            = interpolateLogLinearDf;
    return p;
}

CurveConstructionParams curveParamsMonotoneConvexOnly(void)
{
    CurveConstructionParams p;
    memset(&p, 0, sizeof(p));
    p.numRegimes                        = 1;
    p.regimes[0].upper_time_boundary    = 1e9;
    p.regimes[0].interp_func            = interpolateMonotoneConvex;
    return p;
}

/* ================================================================== */
/*  OIS robustness helpers                                             */
/* ================================================================== */

void setOisAnchorRate(InterestRateCurve *oisCurve, double anchorRate)
{
    oisCurve->rates[0] = anchorRate;
    if (oisCurve->numNodes > 0)
        oisCurve->dfs[0] = exp(-anchorRate * oisCurve->times[0]);
}

void anchorOisAtCbBoundary(InterestRateCurve *oisCurve)
{
    if (oisCurve->cbSchedule.numMeetings <= 0) return;
    if (oisCurve->numNodes >= MAX_NODES) return;

    int32_t nm = oisCurve->cbSchedule.numMeetings;
    double  t_cb = oisCurve->cbSchedule.meetingTimes[nm - 1];
    double  df_cb = getStepWiseDiscountFactor(oisCurve, t_cb);

    /* Find insertion index: first node strictly after t_cb */
    int32_t k = oisCurve->numNodes;
    for (int32_t i = 0; i < oisCurve->numNodes; i++) {
        if (oisCurve->times[i] > t_cb + 1e-9) { k = i; break; }
    }

    /* If a node already exists at t_cb (within tolerance), just update it */
    if (k > 0 && fabs(oisCurve->times[k - 1] - t_cb) < 1e-9) {
        oisCurve->dfs[k - 1]   = df_cb;
        oisCurve->rates[k - 1] = (t_cb > 0.0) ? (-log(df_cb) / t_cb) : 0.0;
        setupMonotoneConvex(oisCurve);
        return;
    }

    /* Shift all nodes from k onward up by one slot */
    for (int32_t i = oisCurve->numNodes; i > k; i--) {
        oisCurve->times[i] = oisCurve->times[i - 1];
        oisCurve->rates[i] = oisCurve->rates[i - 1];
        oisCurve->dfs[i]   = oisCurve->dfs[i - 1];
    }

    oisCurve->times[k] = t_cb;
    oisCurve->dfs[k]   = df_cb;
    oisCurve->rates[k] = (t_cb > 0.0) ? (-log(df_cb) / t_cb) : 0.0;
    oisCurve->numNodes++;
    setupMonotoneConvex(oisCurve);
}

/* ================================================================== */
/*  Basis curve                                                        */
/* ================================================================== */

static double interpolateBasisSpread(const BasisCurve *basis, double t)
{
    if (basis->numNodes <= 0) return 0.0;
    if (t <= basis->times[0]) return basis->spreads[0];

    int n = basis->numNodes;
    if (t >= basis->times[n - 1]) return basis->spreads[n - 1];

    for (int i = 0; i < n - 1; i++) {
        if (t >= basis->times[i] && t < basis->times[i+1]) {
            double w = (t - basis->times[i]) /
                       (basis->times[i+1] - basis->times[i]);
            return basis->spreads[i] + w * (basis->spreads[i+1] - basis->spreads[i]);
        }
    }
    return basis->spreads[n - 1];
}

void computeBasisCurve(const InterestRateCurve *iborFwd,
                       const InterestRateCurve *ois,
                       BasisCurve              *out)
{
    out->numNodes = iborFwd->numNodes;
    for (int i = 0; i < iborFwd->numNodes; i++) {
        double t      = iborFwd->times[i];
        double r_ibor = iborFwd->rates[i];
        double df_ois = getDiscountFactor(ois, t);
        double r_ois  = (t > 0.0) ? (-log(df_ois) / t) : ois->rates[0];
        double spread = r_ibor - r_ois;
        out->times[i]      = t;
        out->spreads[i]    = spread;
        out->spreadsBps[i] = spread * 10000.0;
    }
}

void constructIborFromOisPlusSpread(const InterestRateCurve *ois,
                                    const BasisCurve        *basis,
                                    InterestRateCurve       *out)
{
    memcpy(out, ois, sizeof(InterestRateCurve));
    for (int i = 0; i < out->numNodes; i++) {
        double t = out->times[i];
        out->rates[i] += interpolateBasisSpread(basis, t);
        out->dfs[i]    = (t > 0.0) ? exp(-out->rates[i] * t) : 1.0;
    }
    setupMonotoneConvex(out);
}

/* ================================================================== */
/*  DV01 and key-rate DV01                                             */
/* ================================================================== */

double computeParallelDV01(const MarketInstrument  *instruments,
                           int32_t                  numInstruments,
                           const InterestRateCurve *oisCurve,
                           double maturity, int32_t frequency)
{
    /* Base par rate */
    InterestRateCurve base;
    memset(&base, 0, sizeof(base));
    base.cbSchedule = ((InterestRateCurve *)oisCurve)->cbSchedule;
    bootstrapCurve(&base, oisCurve, instruments, numInstruments, NULL);
    double rBase = solveParSwapRate(&base, oisCurve, maturity, frequency);

    /* Bumped par rate: shift all instrument rates / prices by +1bp */
    MarketInstrument bumped[MAX_NODES];
    for (int32_t i = 0; i < numInstruments; i++) {
        bumped[i] = instruments[i];
        if (instruments[i].type == FUTURE)
            bumped[i].price -= 0.01;   /* -1bp in price = +1bp in rate */
        else
            bumped[i].rate  += 1e-4;
    }
    InterestRateCurve bumpedCurve;
    memset(&bumpedCurve, 0, sizeof(bumpedCurve));
    bumpedCurve.cbSchedule = base.cbSchedule;
    bootstrapCurve(&bumpedCurve, oisCurve, bumped, numInstruments, NULL);
    double rBumped = solveParSwapRate(&bumpedCurve, oisCurve, maturity, frequency);

    return rBumped - rBase;   /* change in par rate per +1bp parallel shift */
}

void computeKeyRateDV01(const MarketInstrument  *instruments,
                        int32_t                  numInstruments,
                        const InterestRateCurve *oisCurve,
                        double maturity, int32_t frequency,
                        double                  *out_dv01)
{
    InterestRateCurve base;
    memset(&base, 0, sizeof(base));
    bootstrapCurve(&base, oisCurve, instruments, numInstruments, NULL);
    double rBase = solveParSwapRate(&base, oisCurve, maturity, frequency);

    MarketInstrument bumped[MAX_NODES];
    for (int32_t j = 0; j < numInstruments; j++) {
        /* Copy all instruments, bump only instrument j */
        for (int32_t k = 0; k < numInstruments; k++)
            bumped[k] = instruments[k];

        if (instruments[j].type == FUTURE)
            bumped[j].price -= 0.01;
        else
            bumped[j].rate  += 1e-4;

        InterestRateCurve bc;
        memset(&bc, 0, sizeof(bc));
        bootstrapCurve(&bc, oisCurve, bumped, numInstruments, NULL);
        out_dv01[j] = solveParSwapRate(&bc, oisCurve, maturity, frequency) - rBase;
    }
}

/* ================================================================== */
/*  Swap analytics                                                     */
/* ================================================================== */

double computeImpliedForwardRate(const InterestRateCurve *fwdCurve,
                                 double tStart, double tEnd, double dt)
{
    if (dt <= 0.0) return 0.0;
    double dfStart = getDiscountFactor(fwdCurve, tStart);
    double dfEnd   = getDiscountFactor(fwdCurve, tEnd);
    return (dfStart / dfEnd - 1.0) / dt;
}

double calculateLegPV(SwapLeg *leg,
                      const InterestRateCurve *fwdCurve,
                      const InterestRateCurve *oisCurve)
{
    double legPV = 0.0;
    for (int32_t i = 0; i < leg->numPeriods; i++) {
        SwapCashFlow cf      = leg->periods[i];
        double dfDiscount    = getDiscountFactor(oisCurve, cf.paymentTime);
        double rate;
        if (leg->isFixed) {
            rate = cf.fixedRate;
        } else {
            rate = computeImpliedForwardRate(fwdCurve, cf.startTime,
                                             cf.endTime, cf.accrualFraction)
                 + cf.spread;
        }
        legPV += cf.notional * rate * cf.accrualFraction * dfDiscount;
    }
    return legPV;
}

double calculateSwapNPV(VanillaSwap *swap,
                        const InterestRateCurve *fwdCurve,
                        const InterestRateCurve *oisCurve)
{
    return calculateLegPV(&swap->floatingLeg, fwdCurve, oisCurve)
         - calculateLegPV(&swap->fixedLeg,    fwdCurve, oisCurve);
}

double calculateSwapDV01(VanillaSwap *swap,
                         MarketInstrument *marketInstruments,
                         int numInstruments,
                         const InterestRateCurve *oisCurve,
                         double bpBumpSize)
{
    MarketInstrument *upInst = malloc(numInstruments * sizeof(MarketInstrument));
    MarketInstrument *dnInst = malloc(numInstruments * sizeof(MarketInstrument));
    for (int i = 0; i < numInstruments; i++) {
        upInst[i] = dnInst[i] = marketInstruments[i];
        if (marketInstruments[i].type == FUTURE) {
            upInst[i].price -= bpBumpSize * 100.0;
            dnInst[i].price += bpBumpSize * 100.0;
        } else {
            upInst[i].rate += bpBumpSize;
            dnInst[i].rate -= bpBumpSize;
        }
    }

    InterestRateCurve fwdUp, fwdDn;
    memset(&fwdUp, 0, sizeof(fwdUp));
    memset(&fwdDn, 0, sizeof(fwdDn));
    bootstrapCurve(&fwdUp, oisCurve, upInst, numInstruments, NULL);
    bootstrapCurve(&fwdDn, oisCurve, dnInst, numInstruments, NULL);

    double npvUp = calculateSwapNPV(swap, &fwdUp, oisCurve);
    double npvDn = calculateSwapNPV(swap, &fwdDn, oisCurve);
    free(upInst);
    free(dnInst);
    return (npvUp - npvDn) / 2.0;
}

/* ================================================================== */
/*  Python bridge entry points                                         */
/* ================================================================== */

MarketInstrument *create_instrument_pool(int32_t size)
{
    return (MarketInstrument *)calloc(size, sizeof(MarketInstrument));
}

void free_instrument_pool(MarketInstrument *ptr)
{
    free(ptr);
}

void run_calibration_bridge(InterestRateCurve *fwdCurve,
                             InterestRateCurve *oisCurve,
                             MarketInstrument  *instruments,
                             int32_t            numInstruments)
{
    memset(fwdCurve, 0, sizeof(InterestRateCurve));

    /* Anchor OIS short end to deposit rate and insert CB boundary node */
    for (int32_t i = 0; i < numInstruments; i++) {
        if (instruments[i].type == DEPOSIT) {
            setOisAnchorRate(oisCurve, instruments[i].rate);
            break;
        }
    }
    if (oisCurve->cbSchedule.numMeetings > 0)
        anchorOisAtCbBoundary(oisCurve);

    /* OIS curve regime: stepwise CB zone → log-linear long end */
    if (oisCurve->cbSchedule.numMeetings > 0) {
        double t_cb = oisCurve->cbSchedule.meetingTimes[
                          oisCurve->cbSchedule.numMeetings - 1];
        oisCurve->regimes[0].upper_time_boundary = t_cb;
        oisCurve->regimes[0].interp_func         = interpolateStepWiseDF;
        oisCurve->regimes[1].upper_time_boundary = 100.0;
        oisCurve->regimes[1].interp_func         = interpolateLogLinearDf;
        oisCurve->numRegimes = 2;
    } else {
        oisCurve->regimes[0].upper_time_boundary = 100.0;
        oisCurve->regimes[0].interp_func         = interpolateLogLinearDf;
        oisCurve->numRegimes = 1;
    }

    bootstrapCurve(fwdCurve, oisCurve, instruments, numInstruments, NULL);
}

double run_swap_analytics_bridge(double *fixedSchedule, double *floatingSchedule,
                                  int32_t numPeriods, double fixedRate,
                                  InterestRateCurve *fwdCurve,
                                  InterestRateCurve *oisCurve,
                                  double *out_fixed_pv, double *out_float_pv)
{
    (void)floatingSchedule;   /* schedule is derived from fixedSchedule periods */
    SwapCashFlow *fixedCFs = malloc(numPeriods * sizeof(SwapCashFlow));
    SwapCashFlow *floatCFs = malloc(numPeriods * sizeof(SwapCashFlow));

    for (int32_t i = 0; i < numPeriods; i++) {
        double tStart = (i == 0) ? 0.0 : fixedSchedule[i-1];
        double tEnd   = fixedSchedule[i];
        fixedCFs[i] = (SwapCashFlow){
            .startTime       = tStart, .endTime      = tEnd,
            .paymentTime     = tEnd,   .accrualFraction = tEnd - tStart,
            .fixedRate       = fixedRate, .notional   = 10000000.0
        };
        floatCFs[i] = (SwapCashFlow){
            .startTime       = tStart, .endTime      = tEnd,
            .paymentTime     = tEnd,   .accrualFraction = tEnd - tStart,
            .spread          = 0.0,    .notional     = 10000000.0
        };
    }

    VanillaSwap swap = {
        .fixedLeg    = { .periods = fixedCFs, .numPeriods = numPeriods, .isFixed = 1 },
        .floatingLeg = { .periods = floatCFs, .numPeriods = numPeriods, .isFixed = 0 }
    };

    *out_fixed_pv = calculateLegPV(&swap.fixedLeg,    fwdCurve, oisCurve);
    *out_float_pv = calculateLegPV(&swap.floatingLeg, fwdCurve, oisCurve);
    double npv    = calculateSwapNPV(&swap, fwdCurve, oisCurve);

    free(fixedCFs);
    free(floatCFs);
    return npv;
}

/* ================================================================== */
/*  main — calibrate from JSON and print diagnostics                   */
/* ================================================================== */

int32_t main(int32_t argc, char **argv)
{
    const char *inputFile       = "in_files/market_data_expanded.json";
    const char *curveAnchorToday = "2026-05-19";
    if (argc > 1) inputFile = argv[1];

    MarketInstrument  marketData[MAX_NODES];
    InterestRateCurve oisCurve, fwdCurve;
    memset(&oisCurve, 0, sizeof(InterestRateCurve));
    memset(&fwdCurve, 0, sizeof(InterestRateCurve));

    int32_t numInstruments = loadInstrumentsFromDatesJSON(
        inputFile, curveAnchorToday, &oisCurve, marketData, MAX_NODES);

    if (numInstruments <= 0) {
        fprintf(stderr, "Fatal: empty file or parse error.\n");
        return -1;
    }

    printf("========================================================================\n");
    printf("   DUAL-CURVE CALIBRATION  (anchor: %s)\n", curveAnchorToday);
    printf("========================================================================\n");
    printf("-> CB meetings parsed:   %d\n", oisCurve.cbSchedule.numMeetings);
    printf("-> OIS curve nodes:      %d\n", oisCurve.numNodes);
    printf("-> Market instruments:   %d\n", numInstruments);

    /* Anchor OIS short end to deposit rate and ensure continuity at CB boundary */
    for (int32_t i = 0; i < numInstruments; i++) {
        if (marketData[i].type == DEPOSIT) {
            setOisAnchorRate(&oisCurve, marketData[i].rate);
            break;
        }
    }
    if (oisCurve.cbSchedule.numMeetings > 0)
        anchorOisAtCbBoundary(&oisCurve);

    double finalMeetingTime =
        oisCurve.cbSchedule.meetingTimes[oisCurve.cbSchedule.numMeetings - 1];

    /* OIS curve regime stack: stepwise for the CB zone, log-linear thereafter */
    oisCurve.regimes[0].upper_time_boundary = finalMeetingTime;
    oisCurve.regimes[0].interp_func         = interpolateStepWiseDF;
    oisCurve.regimes[1].upper_time_boundary = 100.0;
    oisCurve.regimes[1].interp_func         = interpolateLogLinearDf;
    oisCurve.numRegimes = 2;

    printf("\nBootstrapping forward curve...\n");
    if (bootstrapCurve(&fwdCurve, &oisCurve, marketData, numInstruments, NULL) != 0) {
        fprintf(stderr, "bootstrapCurve failed.\n");
        return -1;
    }
    printf("Done. %d nodes.\n", fwdCurve.numNodes);

    printf("\n--- Calibrated Forward Curve ---\n");
    for (int32_t i = 0; i < fwdCurve.numNodes; i++) {
        const char *label = (i < numInstruments)
            ? ((marketData[i].type == DEPOSIT) ? "DEPO"
             : (marketData[i].type == FUTURE)  ? "FUT"  : "SWAP")
            : "NODE";
        printf("  [%2d] t=%5.2fY  %-4s  z=%.4f%%\n",
               i, fwdCurve.times[i], label, fwdCurve.rates[i] * 100.0);
    }

    printf("\n--- OIS-Discounted Par Swap Rates ---\n");
    double tenors[]      = { 2.0, 3.0, 5.0, 10.0, 15.0 };
    int32_t freqs[]      = { 2,   2,   2,   2,    2    };
    int32_t nTenors      = (int32_t)(sizeof(tenors) / sizeof(tenors[0]));

    for (int32_t i = 0; i < nTenors; i++) {
        double par = solveParSwapRate(&fwdCurve, &oisCurve, tenors[i], freqs[i]);
        double mkt = 0.0;
        for (int32_t m = 0; m < numInstruments; m++) {
            if (marketData[m].type == SWAP &&
                fabs(marketData[m].maturity - tenors[i]) < 1e-2) {
                mkt = marketData[m].rate;
                break;
            }
        }
        printf("  %2.0fY  par=%.4f%%", tenors[i], par * 100.0);
        if (mkt > 0.0) printf("  (mkt=%.4f%%)", mkt * 100.0);
        printf("\n");
    }

    printf("\n--- Forward Starting Par Swaps ---\n");
    double fwdStarts[] = { 1.0, 2.0, 5.0 };
    double fwdTenors[] = { 1.0, 1.0, 5.0 };
    const char *labels[] = { "1y1y", "2y1y", "5y5y" };
    int32_t nFwd = (int32_t)(sizeof(fwdStarts) / sizeof(fwdStarts[0]));

    for (int32_t i = 0; i < nFwd; i++) {
        double fpar = solveForwardParSwapRate(
            &fwdCurve, &oisCurve, fwdStarts[i], fwdTenors[i], 2);
        printf("  %-6s  start=%3.1fY  tenor=%3.1fY  par=%.4f%%\n",
               labels[i], fwdStarts[i], fwdTenors[i], fpar * 100.0);
    }

    printf("========================================================================\n");
    return 0;
}
