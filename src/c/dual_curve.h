#ifndef DUAL_CURVE_PRIMARY_H
#define DUAL_CURVE_PRIMARY_H

#include <stdint.h>
#include "date_utils.h"

#ifndef MAX_NODES
#define MAX_NODES 50
#endif

#define MAX_REGIMES  8
#define MAX_HOLIDAYS 500

#define BRENT_MAX_ITER 100
#define BRENT_EPS      1e-12

/* Boundary between log-linear (short/mid) and log-DF cubic (long) regimes.
 * Increase if your futures strip covers more than 2 years. */
#define LOGDF_REGIME_BOUNDARY 2.0

#define NR_DERIV_EPS  1e-5
#define NR_MAX_ITER   25
#define NR_TOLERANCE  1e-12

/* interp.h defines InterpolationFunction, InterpolationRegime, and all
 * interpolation function declarations.  It uses a forward reference to
 * struct InterestRateCurve, so it must be included before the full struct
 * definition below. */
#include "interp.h"

/* ------------------------------------------------------------------ */

typedef struct {
    double  meetingTimes[MAX_NODES];
    double  targetRates[MAX_NODES];
    int32_t numMeetings;
} CentralBankSchedule;

typedef enum {
    DEPOSIT    = 0,
    FUTURE,
    SWAP,
    OIS_SWAP,
    ASSET_SWAP, /* priced against calibrated curve; not a bootstrap calibration input */
    FX_SWAP     /* FX swap: implies foreign DF from domestic DF and forward outright
                 * Encoded via MarketInstrument fields:
                 *   rate     = FX spot     (units of domestic per 1 unit foreign)
                 *   price    = FX forward outright at maturity (same units)
                 *   maturity = swap tenor in years
                 * The bootstrap treats disc curve as the *domestic* reference:
                 *   DF_for(T) = DF_dom(T) * spot / forward
                 * Foreign zero rate = -log(DF_for(T)) / T is written to the fwd curve.
                 */
} InstrumentType;

/* Rate index convention — controls how a floating coupon is observed and paid.
 * Zero-initialised defaults map to IBOR_TERM with no lags (current behaviour). */
typedef enum {
    RATE_IDX_IBOR_TERM    = 0, /* forward-looking term rate (LIBOR, BKBM, EURIBOR) */
    RATE_IDX_OIS_COMPOUND,     /* overnight compound in-arrears (SOFR, SONIA, ESTR) */
    RATE_IDX_OIS_AVERAGE,      /* overnight simple-average (SOFR Averages) */
    RATE_IDX_TERM_SOFR         /* CME Term SOFR — forward-looking, priced like IBOR */
} RateIndexType;

typedef struct {
    RateIndexType         indexType;       /* default 0 = IBOR_TERM                 */
    int                   _pad0;           /* explicit align to double              */
    double                tenorYears;      /* observation tenor: 0.25=3M, 0.5=6M    */
    int                   resetLagDays;    /* biz days before period start (-2=IBOR) */
    int                   paymentLagDays;  /* biz days after period end              */
    int                   lookbackDays;    /* SOFR: shift obs window back N days     */
    int                   lockoutDays;     /* SOFR: freeze last N days of window     */
    DayCountFraction      dcf;
    BusinessDayAdjustment bda;
    char                  calendarName[32];
} FloatingRateIndex;

/* -----------------------------------------------------------------
 * MarketInstrument is a tagged union.
 *   `type`       — selects which member of `spec` is live
 *   `startTime`  — common: start-of-period year fraction (0 for spot-start)
 *   `maturity`   — common: end-of-period year fraction
 *   `spec.*`     — variant-specific fields
 *
 * DO NOT read the wrong variant. Consumers must switch on `type`
 * before touching any spec field.
 * ----------------------------------------------------------------- */
typedef struct {
    double                rate;              /* deposit rate                    */
    DayCountFraction      dcf;               /* day count (0 = Act/365)         */
    BusinessDayAdjustment bda;               /* business-day convention         */
    char                  calendarName[32];  /* e.g. "USD"                      */
} DepositSpec;

typedef struct {
    double                price;             /* futures quote (100 - fwd*100)   */
    DayCountFraction      dcf;               /* accrual day count               */
    int                   _pad;              /* explicit align to char array    */
    char                  calendarName[32];
} FutureSpec;

typedef struct {
    double                rate;              /* par swap rate                   */
    int32_t               paymentFrequency;  /* fixed-leg payments per year     */
    DayCountFraction      fixedDcf;
    DayCountFraction      floatDcf;
    BusinessDayAdjustment bda;
    char                  calendarName[32];
    FloatingRateIndex     floatIndex;        /* full float-leg conventions      */
} SwapSpec;                                  /* also used for OIS_SWAP          */

typedef struct {
    double                fxSpot;            /* dom per for at inception        */
    double                fxForward;         /* dom per for at maturity         */
    char                  calendarName[32];
} FxSwapSpec;

typedef union {
    DepositSpec deposit;
    FutureSpec  future;
    SwapSpec    swap;    /* SWAP + OIS_SWAP  */
    FxSwapSpec  fxSwap;
} InstrumentSpec;

typedef struct {
    InstrumentType type;
    int            _pad0;      /* align startTime to 8                */
    double         startTime;
    double         maturity;
    InstrumentSpec spec;
} MarketInstrument;

typedef struct InterestRateCurve {
    int32_t numNodes;
    double  times[MAX_NODES];
    double  rates[MAX_NODES];
    double  dfs[MAX_NODES];

    CentralBankSchedule cbSchedule;

    InterpolationRegime regimes[MAX_REGIMES];
    int32_t             numRegimes;

    double spline_a[MAX_NODES];
    double spline_b[MAX_NODES];
    double spline_c[MAX_NODES];
    double spline_d[MAX_NODES];
} InterestRateCurve;

/* Used internally by the swap bootstrap solver */
typedef struct {
    InterestRateCurve       *forwardCurve;
    const InterestRateCurve *discountCurve;
    MarketInstrument        *inst;
    int32_t                  currentNodeIdx;
} CalibrationContext;

typedef struct {
    double startTime;
    double endTime;
    double paymentTime;      /* may differ from endTime when paymentLagDays != 0 */
    double accrualFraction;
    double fixedRate;
    double spread;
    double notional;
    double resetTime;        /* rate fixing date; 0 → use startTime */
    double obsWindowStart;   /* compounding/averaging window start; 0 → startTime */
    double obsWindowEnd;     /* compounding/averaging window end; 0 → endTime */
} SwapCashFlow;

typedef struct {
    SwapCashFlow *periods;
    int32_t       numPeriods;
    int32_t       isFixed;
    RateIndexType rateConv;   /* floating-rate convention; 0 = IBOR_TERM (default) */
} SwapLeg;

typedef struct {
    SwapLeg fixedLeg;
    SwapLeg floatingLeg;
} VanillaSwap;

/* --- Convexity adjustment parameters (Hull-White approx) ----------- */
typedef struct {
    double sigma;  /* short-rate volatility; 0.0 = no adjustment */
} ConvexityParams;

/* --- Curve construction parameters ---------------------------------- */
typedef struct {
    /* Custom regime stack.  numRegimes == 0 → use built-in defaults. */
    InterpolationRegime regimes[MAX_REGIMES];
    int32_t             numRegimes;

    /* Futures convexity adjustment.  sigma == 0.0 → no adjustment. */
    ConvexityParams convexity;
} CurveConstructionParams;

/* --- Basis curve (IBOR zero rate minus OIS zero rate) --------------- */
typedef struct {
    int32_t numNodes;
    double  times[MAX_NODES];
    double  spreads[MAX_NODES];     /* continuous zero-rate spread */
    double  spreadsBps[MAX_NODES];  /* spread * 10000 */
} BasisCurve;

/* ================================================================== */
/*  Function declarations                                              */
/* ================================================================== */

/* Schedule builder: fills out[0..n-1] for a vanilla swap leg.
 * Returns number of periods written, or -1 on error.
 * floatIdx == NULL → zero lags, IBOR_TERM (identical to current behaviour). */
int buildSwapSchedule(SwapCashFlow            *out,
                      int                      maxPeriods,
                      double                   startTime,
                      double                   maturity,
                      int                      frequency,
                      double                   fixedRateOrSpread,
                      double                   notional,
                      int                      isFixed,
                      const FloatingRateIndex *floatIdx);

/* OIS robustness: anchor short-end rate and insert continuity node */
void setOisAnchorRate(InterestRateCurve *oisCurve, double anchorRate);
void anchorOisAtCbBoundary(InterestRateCurve *oisCurve);

/* Bootstrap engine.
 * Returns 0 on success, -1 if MAX_NODES would be exceeded.
 * params == NULL → use built-in three-regime default layout. */
int bootstrapCurve(InterestRateCurve             *fwdCurve,
                   const InterestRateCurve        *oisCurve,
                   const MarketInstrument         *instruments,
                   int32_t                         numInstruments,
                   const CurveConstructionParams  *params);

/* OIS curve bootstrap (self-discounted; wraps bootstrapCurve). */
int bootstrapOisCurve(InterestRateCurve            *oisCurve,
                      const MarketInstrument        *instruments,
                      int32_t                        numInstruments,
                      const CurveConstructionParams *params);

/* Pre-built parameter sets for common interpolation layouts */
CurveConstructionParams curveParamsDefault(void);
CurveConstructionParams curveParamsLogLinearOnly(void);
CurveConstructionParams curveParamsMonotoneConvexOnly(void);

/* Par swap rate under dual-curve discounting */
double solveParSwapRate(const InterestRateCurve *fwdCurve,
                        const InterestRateCurve *oisCurve,
                        double maturity, int32_t frequency);

double solveForwardParSwapRate(const InterestRateCurve *fwdCurve,
                               const InterestRateCurve *oisCurve,
                               double forwardStart, double swapTenor,
                               int32_t frequency);

/* Basis curve */
void computeBasisCurve(const InterestRateCurve *iborFwd,
                       const InterestRateCurve *ois,
                       BasisCurve              *out);

void constructIborFromOisPlusSpread(const InterestRateCurve *ois,
                                    const BasisCurve        *basis,
                                    InterestRateCurve       *out);

/* DV01 */
double computeParallelDV01(const MarketInstrument  *instruments,
                           int32_t                  numInstruments,
                           const InterestRateCurve *oisCurve,
                           double maturity, int32_t frequency);

void computeKeyRateDV01(const MarketInstrument  *instruments,
                        int32_t                  numInstruments,
                        const InterestRateCurve *oisCurve,
                        double maturity, int32_t frequency,
                        double                  *out_dv01);

/* Swap analytics */

/* Project a floating coupon rate from projCurve using the given convention.
 * When cf->resetTime/obsWindowStart/obsWindowEnd are zero, falls back to
 * (startTime, endTime) — identical to computeImpliedForwardRate. */
double computeFloatRate(const SwapCashFlow      *cf,
                        const InterestRateCurve *projCurve,
                        RateIndexType            conv);

double computeImpliedForwardRate(const InterestRateCurve *fwdCurve,
                                 double tStart, double tEnd, double dt);
double calculateLegPV(SwapLeg *leg,
                      const InterestRateCurve *fwdCurve,
                      const InterestRateCurve *oisCurve);
double calculateSwapNPV(VanillaSwap *swap,
                        const InterestRateCurve *fwdCurve,
                        const InterestRateCurve *oisCurve);
double calculateSwapDV01(VanillaSwap *swap,
                         MarketInstrument *marketInstruments,
                         int numInstruments,
                         const InterestRateCurve *oisCurve,
                         double bpBumpSize);

/* Python bridge entry points */
MarketInstrument *create_instrument_pool(int32_t size);
void              free_instrument_pool(MarketInstrument *ptr);
void              run_calibration_bridge(InterestRateCurve  *fwdCurve,
                                         InterestRateCurve  *oisCurve,
                                         MarketInstrument   *instruments,
                                         int32_t             numInstruments);
double            run_swap_analytics_bridge(double *fixedSchedule,
                                            double *floatingSchedule,
                                            int32_t numPeriods,
                                            double  fixedRate,
                                            InterestRateCurve *fwdCurve,
                                            InterestRateCurve *oisCurve,
                                            double *out_fixed_pv,
                                            double *out_float_pv);

#endif /* DUAL_CURVE_PRIMARY_H */
