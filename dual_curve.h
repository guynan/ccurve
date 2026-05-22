
#ifndef DUAL_CURVE_PRIMARY_H
#define DUAL_CURVE_PRIMARY_H


#define MAX_NODES 50
#define BRENT_MAX_ITER 100
#define BRENT_EPS 1e-12


/* Boundary between log-linear (short) and log-DF cubic (long) regimes.
 * Adjust if your futures strip covers more than 2 years.               */
#define LOGDF_REGIME_BOUNDARY 2.0

/* Epsilon for numerical derivative in N-R  (0.1 basis point)          */
#define NR_DERIV_EPS 1e-5

/* Maximum Newton-Raphson iterations and convergence tolerance          */
#define NR_MAX_ITER  25
#define NR_TOLERANCE 1e-12


typedef struct {
	double meetingTimes[MAX_NODES];
	double targetRates[MAX_NODES]; // Explicit absolute rate storage (e.g., 0.0425)
	int32_t numMeetings;
} CentralBankSchedule;

typedef enum {
	DEPOSIT,
	FUTURE,
	SWAP
} InstrumentType;

typedef struct {
	InstrumentType type;
	double startTime;
	double maturity;
	double rate;
	double price;
	int32_t paymentFrequency;
} MarketInstrument;

struct InterestRateCurve;
typedef double (*InterpolationFunction)(struct InterestRateCurve *curve, double t, int32_t idx);

typedef struct {
	double upper_time_boundary;		 
	InterpolationFunction interp_func;
} InterpolationRegime;

typedef struct InterestRateCurve {
	int32_t numNodes;
	double times[MAX_NODES];
	double rates[MAX_NODES];
	double dfs[MAX_NODES];
	
	CentralBankSchedule cbSchedule;

	InterpolationRegime regimes[5];
	int32_t numRegimes;

	double spline_a[MAX_NODES];
	double spline_b[MAX_NODES];
	double spline_c[MAX_NODES];
	double spline_d[MAX_NODES];
} InterestRateCurve;

// CalibrationContext expanded to handle dual-curve execution structures
typedef struct {
	InterestRateCurve *forwardCurve;  // Curve we are actively solving for
	InterestRateCurve *discountCurve; // Static read-only OIS curve used for PV
	MarketInstrument *inst;
	int32_t currentNodeIdx;
} CalibrationContext;

// Represents a single localized cash flow event
typedef struct {
	double startTime;	   // Accrual start for the period
	double endTime;		 // Accrual end for the period
	double paymentTime;	 // Actual cash settlement timeline node
	double accrualFraction; // Day count fraction (e.g., 0.25 for quarterly)
	double fixedRate;	   // Populated for Fixed Legs
	double spread;		  // Added to forward rates on Floating Legs
	double notional;		// Principal balance base
} SwapCashFlow;

// A collection of cash flows representing one side of the trade
typedef struct {
	SwapCashFlow* periods;
	int32_t numPeriods;
	int32_t isFixed;			// 1 = Fixed Leg, 0 = Floating Leg
} SwapLeg;

// The complete trade container
typedef struct {
	SwapLeg fixedLeg;
	SwapLeg floatingLeg;
} VanillaSwap;

double getDiscountFactor(InterestRateCurve *curve, double t);

#endif
