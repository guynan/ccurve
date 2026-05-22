#ifndef DUAL_CURVE_PRIMARY_H
#define DUAL_CURVE_PRIMARY_H

#include <stdint.h>

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
    DEPOSIT  = 0,
    FUTURE,
    SWAP,
    OIS_SWAP
} InstrumentType;

typedef struct {
    InstrumentType type;
    double         startTime;
    double         maturity;
    double         rate;
    double         price;
    int32_t        paymentFrequency;
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
    double paymentTime;
    double accrualFraction;
    double fixedRate;
    double spread;
    double notional;
} SwapCashFlow;

typedef struct {
    SwapCashFlow *periods;
    int32_t       numPeriods;
    int32_t       isFixed;
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
