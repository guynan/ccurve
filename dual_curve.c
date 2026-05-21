#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>  // C99 ptrdiff_t compliance


#include "date_utils.h"

#define MAX_NODES 50
#define BRENT_MAX_ITER 100
#define BRENT_EPS 1e-12

// ==========================================
// 1. Core Data Structures & Architecture
// ==========================================

typedef struct {
	double meetingTimes[MAX_NODES];
	double targetRates[MAX_NODES]; // Explicit absolute rate storage (e.g., 0.0425)
	int numMeetings;
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
	int paymentFrequency;
} MarketInstrument;

struct InterestRateCurve;
typedef double (*InterpolationFunction)(struct InterestRateCurve *curve, double t, int idx);

typedef struct {
	double upper_time_boundary;		 
	InterpolationFunction interp_func;
} InterpolationRegime;

typedef struct InterestRateCurve {
	int numNodes;
	double times[MAX_NODES];
	double rates[MAX_NODES];
	double dfs[MAX_NODES];
	
	CentralBankSchedule cbSchedule;

	InterpolationRegime regimes[5];
	int numRegimes;

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
	int currentNodeIdx;
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
	int numPeriods;
	int isFixed;			// 1 = Fixed Leg, 0 = Floating Leg
} SwapLeg;

// The complete trade container
typedef struct {
	SwapLeg fixedLeg;
	SwapLeg floatingLeg;
} VanillaSwap;

double getDiscountFactor(InterestRateCurve *curve, double t);

// ==========================================
// 2. Concrete Interpolation Engines
// ==========================================

double interpolateLogLinearDf(InterestRateCurve *curve, double t, int idx) {
	double t0 = curve->times[idx], t1 = curve->times[idx+1];
	double df0 = curve->dfs[idx], df1 = curve->dfs[idx+1];
	if (df0 <= 0.0 || df1 <= 0.0) return 0.0;
	return exp(log(df0) + (t - t0) / (t1 - t0) * (log(df1) - log(df0)));
}

double interpolateStepWiseOIS(InterestRateCurve *curve, double t, int idx) {
	int numMeetings = curve->cbSchedule.numMeetings;
	
	// 1. If we are evaluating a time before the first meeting, 
	// use the curve's front anchor rate (Today's effective overnight rate)
	if (numMeetings == 0 || t < curve->cbSchedule.meetingTimes[0]) {
		return curve->rates[0];
	}
	
	// 2. Step through the schedule to find which meeting bracket time 't' falls into
	for (int i = 0; i < numMeetings - 1; i++) {
		if (t >= curve->cbSchedule.meetingTimes[i] && t < curve->cbSchedule.meetingTimes[i+1]) {
			return curve->cbSchedule.targetRates[i];
		}
	}
	
	// 3. If 't' is past the last scheduled meeting, hold the final target rate flat
	return curve->cbSchedule.targetRates[numMeetings - 1];
}

double getStepWiseDiscountFactor(InterestRateCurve *curve, double t) {
	int numMeetings = curve->cbSchedule.numMeetings;
	if (t <= 0.0) return 1.0;
	
	double integratedRateTime = 0.0;
	double tCurrent = 0.0;
	
	// Integrate across the steps up to time 't'
	for (int i = 0; i < numMeetings; i++) {
		double tMeeting = curve->cbSchedule.meetingTimes[i];
		
		if (t <= tMeeting) {
			// The requested time falls before the next meeting
			double activeRate = (i == 0) ? curve->rates[0] : curve->cbSchedule.targetRates[i-1];
			integratedRateTime += activeRate * (t - tCurrent);
			tCurrent = t;
			break;
		} else {
			// The requested time is past this meeting; integrate the full bracket
			double activeRate = (i == 0) ? curve->rates[0] : curve->cbSchedule.targetRates[i-1];
			integratedRateTime += activeRate * (tMeeting - tCurrent);
			tCurrent = tMeeting;
		}
	}
	
	// If 't' extends past the final meeting, add the remaining tail period
	if (t > tCurrent) {
		double finalRate = curve->cbSchedule.targetRates[numMeetings-1];
		integratedRateTime += finalRate * (t - tCurrent);
	}
	
	return exp(-integratedRateTime);
}

void setupParabolicSpline(InterestRateCurve *curve) {
	int n = curve->numNodes - 1;
	if (n < 1) return;
	for (int i = 0; i <= n; i++) curve->spline_a[i] = curve->rates[i];
	
	double initial_slope = (curve->rates[1] - curve->rates[0]) / (curve->times[1] - curve->times[0]);
	curve->spline_b[0] = initial_slope;

	for (int i = 0; i < n; i++) {
		double h = curve->times[i+1] - curve->times[i];
		curve->spline_c[i] = (curve->rates[i+1] - curve->spline_a[i] - curve->spline_b[i] * h) / (h * h);
		if (i < n - 1) {
			curve->spline_b[i+1] = curve->spline_b[i] + 2.0 * curve->spline_c[i] * h;
		}
		curve->spline_d[i] = 0.0;
	}
}

double interpolateParabolicZero(InterestRateCurve *curve, double t, int idx) {
	double dx = t - curve->times[idx];
	double r = curve->spline_a[idx] + curve->spline_b[idx] * dx + curve->spline_c[idx] * dx * dx;
	return exp(-r * t);
}

void setupCubicSpline(InterestRateCurve *curve) {
	int n = curve->numNodes - 1;
	if (n < 2) return;
	double h[MAX_NODES], alpha[MAX_NODES], l[MAX_NODES], mu[MAX_NODES], z[MAX_NODES];

	for (int i = 0; i <= n; i++) curve->spline_a[i] = curve->rates[i];
	for (int i = 0; i < n; i++)  h[i] = curve->times[i+1] - curve->times[i];

	for (int i = 1; i < n; i++) {
		alpha[i] = (3.0 / h[i]) * (curve->spline_a[i+1] - curve->spline_a[i]) - 
				   (3.0 / h[i-1]) * (curve->spline_a[i] - curve->spline_a[i-1]);
	}

	l[0] = 1.0;
	mu[0] = 0.0;
	z[0] = 0.0;
	l[n] = 1.0;
	z[n] = 0.0;
	curve->spline_c[n] = 0.0;

	for (int i = 1; i < n; i++) {
		l[i] = 2.0 * (curve->times[i+1] - curve->times[i-1]) - h[i-1] * mu[i-1];
		mu[i] = h[i] / l[i];
		z[i] = (alpha[i] - h[i-1] * z[i-1]) / l[i];
	}

	for (int j = n - 1; j >= 0; j--) {
		curve->spline_c[j] = z[j] - mu[j] * curve->spline_c[j+1];
		curve->spline_b[j] = (curve->spline_a[j+1] - curve->spline_a[j]) / h[j] - 
							 h[j] * (curve->spline_c[j+1] + 2.0 * curve->spline_c[j]) / 3.0;
		curve->spline_d[j] = (curve->spline_c[j+1] - curve->spline_c[j]) / (3.0 * h[j]);
	}
}

double interpolateCubicZero(InterestRateCurve *curve, double t, int idx) {
	if (curve->numNodes < 3) return interpolateLogLinearDf(curve, t, idx);
	double dx = t - curve->times[idx];
	double r = curve->spline_a[idx] + curve->spline_b[idx] * dx +
			   curve->spline_c[idx] * dx * dx + curve->spline_d[idx] * dx * dx * dx;
	return exp(-r * t);
}


double getDiscountFactor(InterestRateCurve *curve, double t) {
	if (t <= 0.0) return 1.0;
	if (t <= curve->times[0]) return exp(-curve->rates[0] * t);
	
	// Inside your central getDiscountFactor function:
	if (curve->numRegimes > 0 && t <= curve->regimes[0].upper_time_boundary) {
		return getStepWiseDiscountFactor(curve, t);
	}

	int idx = 0;
	if (t >= curve->times[curve->numNodes - 1]) {
		idx = curve->numNodes - 2;
	} else {
		int low = 0, high = curve->numNodes - 1;
		while (high - low > 1) {
			int mid = (low + high) / 2;
			if (curve->times[mid] > t) high = mid;
			else low = mid;
		}
		idx = low;
	}

	for (int i = 0; i < curve->numRegimes; i++) {
		if (t <= curve->regimes[i].upper_time_boundary || i == curve->numRegimes - 1) {
			return curve->regimes[i].interp_func(curve, t, idx);
		}
	}
	return interpolateLogLinearDf(curve, t, idx);
}

// ==========================================
// 4. Mathematical Optimization (Brent's Solver)
// ==========================================

double brentsMethod(double ax, double bx, double cx, double (*f)(double, void*), void *params) {
	double a = ax, b = bx, c = cx;
	double fa = f(a, params); 
	double fb = f(b, params);
	
	int expansions = 0;
	while ((fa * fb > 0.0) && expansions < 10) {
		a -= 0.05;
		b += 0.05;
		fa = f(a, params);
		fb = f(b, params);
		expansions++;
	}
	
	c = a;
	double fc = fa;
	double d = b - a, e = d;
	double p, q, r, s, tol1, xm;

	for (int iter = 1; iter <= BRENT_MAX_ITER; iter++) {
		if ((fb > 0.0 && fc > 0.0) || (fb < 0.0 && fc < 0.0)) {
			c = a;
			fc = fa;
			e = d = b - a;
		}
		if (fabs(fc) < fabs(fb)) {
			a = b;
			b = c;
			c = a;
			double tmp = fa; // Structural Swap correction using temp container
			fa = fb;
			fb = fc;
			fc = tmp;
		}
		tol1 = 2.0 * BRENT_EPS * fabs(b) + 0.5 * BRENT_EPS;
		xm = 0.5 * (c - b);
		if (fabs(xm) <= tol1 || fb == 0.0) return b;
		
		if (fabs(e) >= tol1 && fabs(fa) > fabs(fb)) {
			s = fb / fa;
			if (a == c) {
				p = 2.0 * xm * s;
				q = 1.0 - s;
			} else {
				q = fa / fc;
				r = fb / fc;
				p = s * (2.0 * xm * q * (q - r) - (b - a) * (r - 1.0));
				q = (q - 1.0) * (r - 1.0) * (s - 1.0);
			}
			if (p > 0.0) q = -q;
			p = fabs(p);
			double min1 = 3.0 * xm * q - fabs(tol1 * q);
			double min2 = fabs(e * q);
			if (2.0 * p < (min1 < min2 ? min1 : min2)) {
				e = d; d = p / q;
			} else {
				d = xm; e = d;
			}
		} else {
			d = xm; e = d;
		}
		a = b;
		fa = fb;
		if (fabs(d) > tol1) b += d;
		else b += (xm > 0.0 ? tol1 : -tol1);
		fb = f(b, params);
	}
	return b;
}

// ==========================================
// 5. Objective Pricing Functions (Dual-Curve Realignment)
// ==========================================

double objectiveFunction(double trialRate, void *params) {
	CalibrationContext *ctx = (CalibrationContext*)params;
	InterestRateCurve *fwdCurve = ctx->forwardCurve;
	MarketInstrument *inst = ctx->inst;
	int nodeIdx = ctx->currentNodeIdx;
	
	fwdCurve->rates[nodeIdx] = trialRate;
	fwdCurve->dfs[nodeIdx] = exp(-trialRate * fwdCurve->times[nodeIdx]);
	
	setupCubicSpline(fwdCurve);
	setupParabolicSpline(fwdCurve);

	if (inst->type == DEPOSIT) {
		double dfFwdEnd = getDiscountFactor(fwdCurve, inst->maturity);
		double impliedDepoRate = (1.0 / dfFwdEnd - 1.0) / inst->maturity;
		return impliedDepoRate - inst->rate;
	} 
	else if (inst->type == FUTURE) {
		double tStart = inst->startTime;
		double tEnd = inst->maturity;
		double dt = tEnd - tStart;
		
		double dfFwdStart = getDiscountFactor(fwdCurve, tStart);
		double dfFwdEnd = getDiscountFactor(fwdCurve, tEnd);
		double impliedFwd = (dfFwdStart / dfFwdEnd - 1.0) / dt;
		
		double targetRate = 1.0 - (inst->price / 100.0);
		return impliedFwd - targetRate;
	} 
	else { // SWAP (True Dual Curve Core Math Execution)
		double pvFloating = 0.0;
		double pvFixed = 0.0;
		double dt = 1.0 / inst->paymentFrequency;
		int numPayments = (int)(inst->maturity * inst->paymentFrequency + 0.5);
		
		double tPrev = 0.0;
		for (int i = 1; i <= numPayments; i++) {
			double tPay = i * dt;
			
			double dfFwdPay = getDiscountFactor(fwdCurve, tPay);
			double dfFwdPrev = getDiscountFactor(fwdCurve, tPrev);
			double fwdRate = (dfFwdPrev / dfFwdPay - 1.0) / dt;
			
			double dfOisPay = getDiscountFactor(ctx->discountCurve, tPay);
			
			pvFloating += fwdRate * dt * dfOisPay;
			pvFixed += inst->rate * dt * dfOisPay;
			
			tPrev = tPay;
		}
		return pvFloating - pvFixed;
	}
}

double solveParSwapRate(InterestRateCurve *fwdCurve, InterestRateCurve *oisCurve, double maturity, int frequency) 
{
	double dt = 1.0 / frequency;
	int payments = (int)(maturity * frequency + 0.5);
	double floatingLeg = 0.0;
	double fixedAnnuity = 0.0;
	
	double tPrev = 0.0;
	for (int i = 1; i <= payments; i++) {
		double tPay = i * dt;
		double dfFwdPay = getDiscountFactor(fwdCurve, tPay);
		double dfFwdPrev = getDiscountFactor(fwdCurve, tPrev);
		double fwdRate = (dfFwdPrev / dfFwdPay - 1.0) / dt;
		
		double dfOisPay = getDiscountFactor(oisCurve, tPay);
		
		floatingLeg += fwdRate * dt * dfOisPay;
		fixedAnnuity += dt * dfOisPay;
		tPrev = tPay;
	}
	return floatingLeg / fixedAnnuity;
}

// ==========================================
// 6. Bootstrap Orchestration Matrix
// ==========================================

void bootstrapCurve(InterestRateCurve *fwdCurve, InterestRateCurve *oisCurve, 
					MarketInstrument instruments[], int numInstruments) 
{
	fwdCurve->numNodes = 0;
	
	fwdCurve->times[0] = 0.0;
	fwdCurve->rates[0] = instruments[0].rate; 
	fwdCurve->dfs[0] = 1.0;
	fwdCurve->numNodes = 1;

	for (int i = 0; i < numInstruments; i++) {
		MarketInstrument *inst = &instruments[i];
		int idx = fwdCurve->numNodes;

		if (inst->type == DEPOSIT) {
			double t = inst->maturity;
			double dt = t - inst->startTime;
			
			double df = 1.0 / (1.0 + inst->rate * dt);
			
			fwdCurve->times[idx] = t;
			fwdCurve->dfs[idx] = df;
			fwdCurve->rates[idx] = (t > 0.0) ? -log(df) / t : inst->rate;
			fwdCurve->numNodes++;
		}
		else if (inst->type == FUTURE) {
			double t_start = inst->startTime;
			double t_end = inst->maturity;
			double delta_t = t_end - t_start;
			
			double impliedFwdRate = (100.0 - inst->price) / 100.0;
			double df_start = getDiscountFactor(fwdCurve, t_start);
			double df_end = df_start / (1.0 + impliedFwdRate * delta_t);
			
			fwdCurve->times[idx] = t_end;
			fwdCurve->dfs[idx] = df_end;
			fwdCurve->rates[idx] = -log(df_end) / t_end;
			fwdCurve->numNodes++;
		}
		else if (inst->type == SWAP) {
			double t_maturity = inst->maturity;
			double swapRate = inst->rate;
			int freq = inst->paymentFrequency;
			
			int totalPeriods = (int)round(t_maturity * freq);
			double dt = 1.0 / (double)freq;
			
			double low_rate = -0.05; 
			double high_rate = 0.50; 
			double guessed_zero_rate = 0.0;
			double df_end_solved = 1.0;
			
			int max_iterations = 100;
			double tolerance = 1e-9;
			
			for (int iter = 0; iter < max_iterations; iter++) {
				guessed_zero_rate = 0.5 * (low_rate + high_rate);
				
				fwdCurve->times[idx] = t_maturity;
				fwdCurve->rates[idx] = guessed_zero_rate;
				fwdCurve->dfs[idx] = exp(-guessed_zero_rate * t_maturity);
				fwdCurve->numNodes = idx + 1; 
				
				double floatingLegPV = 0.0;
				double fixedLegPV = 0.0;
				
				for (int p = 1; p <= totalPeriods; p++) {
					double t_pay = p * dt;
					double t_start = t_pay - dt;
					
					double df_ois_discount = getDiscountFactor(oisCurve, t_pay);
					fixedLegPV += swapRate * dt * df_ois_discount;
					
					double df_fwd_start = getDiscountFactor(fwdCurve, t_start);
					double df_fwd_end   = getDiscountFactor(fwdCurve, t_pay);
					double impliedFwd   = (df_fwd_start / df_fwd_end - 1.0) / dt;
					
					floatingLegPV += impliedFwd * dt * df_ois_discount;
				}
				
				double npv_residual = floatingLegPV - fixedLegPV;
				if (fabs(npv_residual) < tolerance) {
					df_end_solved = fwdCurve->dfs[idx];
					break;
				}
				
				if (npv_residual > 0.0) {
					high_rate = guessed_zero_rate;
				} else {
					low_rate = guessed_zero_rate;
				}
			}
			
			fwdCurve->times[idx] = t_maturity;
			fwdCurve->rates[idx] = guessed_zero_rate;
			fwdCurve->dfs[idx] = df_end_solved;
			fwdCurve->numNodes = idx + 1;
		}
	}
}

// ==========================================
// 7. Streaming Dual Curve JSON Tokenizer
// ==========================================

int loadInstrumentsFromDatesJSON(const char *filename, const char* anchorDateStr,
								 InterestRateCurve *oisCurve, MarketInstrument instruments[], int maxInstruments) {
	FILE *file = fopen(filename, "r");
	if (!file) {
		printf("Error: Could not open market layout data file %s\n", filename);
		return -1;
	}

	DateTime anchorDate = parseDateString(anchorDateStr);
	char line[256];
	int instCount = 0;
	int oisCount = 0;
	int cbCount = 0;

	// Explicit state tracking for sections
	int readingMeetings = 0;
	int readingOis = 0;
	int readingMarket = 0;

	char typeStr[32] = "";
	char startExStr[32] = "";
	char matExStr[32] = "";
	char oisDateStr[32] = "";
	char meetingDateStr[32] = "";
	
	double rate = -1.0, price = -1.0, targetRateInput = -1.0;
	int frequency = -1;

	while (fgets(line, sizeof(line), file)) {
		
		// ==========================================
		// 1. Section Header Detection
		// ==========================================
		if (strstr(line, "\"meeting_schedule\"")) {
			readingMeetings = 1;
			readingOis = 0;
			readingMarket = 0;
			continue;
		}
		if (strstr(line, "\"ois_curve\"")) {
			readingOis = 1;
			readingMeetings = 0;
			readingMarket = 0;
			continue;
		}
		if (strstr(line, "\"market_data\"")) { 
			readingMarket = 1;
			readingOis = 0;
			readingMeetings = 0;
			continue;
		}

		// ==========================================
		// 2. Central Bank Meetings Parser
		// ==========================================
		if (readingMeetings) {
			char *dateKey = strstr(line, "\"date\"");
			char *rateKey = strstr(line, "\"target_rate\"");
			
			if (dateKey) {
				if (sscanf(dateKey, "\"date\" : \"%10[^\"]\"", meetingDateStr) != 1) {
					sscanf(dateKey, "\"date\":\"%10[^\"]\"", meetingDateStr);
				}
			}
			if (rateKey) {
				char *colon = strchr(rateKey, ':');
				if (colon) targetRateInput = strtod(colon + 1, NULL);
			}
			if (strstr(line, "}") && cbCount < MAX_NODES && strlen(meetingDateStr) > 0) {
				DateTime mtDate = parseDateString(meetingDateStr);
				double t = calculateYearFraction(anchorDate, mtDate);
				
				oisCurve->cbSchedule.meetingTimes[cbCount] = t;
				oisCurve->cbSchedule.targetRates[cbCount] = targetRateInput;
				cbCount++;
				
				meetingDateStr[0] = '\0';
				targetRateInput = -1.0;
			}
		}
		// ==========================================
		// 3. OIS Term Curve Nodes Parser
		// ==========================================
		else if (readingOis) {
			char *dateKey = strstr(line, "\"date\"");
			char *rateKey = strstr(line, "\"rate\"");
			
			if (dateKey) {
				if (sscanf(dateKey, "\"date\" : \"%10[^\"]\"", oisDateStr) != 1) {
					sscanf(dateKey, "\"date\":\"%10[^\"]\"", oisDateStr);
				}
			}
			if (rateKey) {
				char *colon = strchr(rateKey, ':');
				if (colon) rate = strtod(colon + 1, NULL);
			}
			if (strstr(line, "}") && oisCount < MAX_NODES && strlen(oisDateStr) > 0) {
				DateTime targetOisDate = parseDateString(oisDateStr);
				double t = calculateYearFraction(anchorDate, targetOisDate);

				oisCurve->times[oisCount] = t;
				oisCurve->rates[oisCount] = rate;
				oisCurve->dfs[oisCount] = exp(-rate * t);
				oisCount++;

				oisDateStr[0] = '\0';
				rate = -1.0;
			}
		}
		// ==========================================
		// 4. Market Instruments Array Parser
		// ==========================================
		else if (readingMarket) {
			char *typeKey = strstr(line, "\"type\"");
			char *startKey = strstr(line, "\"startDate\"");
			char *matKey = strstr(line, "\"maturityDate\"");
			char *rateKey = strstr(line, "\"rate\"");
			char *priceKey = strstr(line, "\"price\"");
			char *freqKey = strstr(line, "\"paymentFrequency\"");

			// Incremental key extractions (handles both spaced and dense JSON variants)
			if (typeKey)  { 
				if (sscanf(typeKey, "\"type\" : \"%15[^\"]\"", typeStr) != 1) {
					sscanf(typeKey, "\"type\":\"%15[^\"]\"", typeStr);
				}
			}
			if (startKey) { 
				if (sscanf(startKey, "\"startDate\" : \"%10[^\"]\"", startExStr) != 1) {
					sscanf(startKey, "\"startDate\":\"%10[^\"]\"", startExStr);
				}
			}
			if (matKey)   { 
				if (sscanf(matKey, "\"maturityDate\" : \"%10[^\"]\"", matExStr) != 1) {
					sscanf(matKey, "\"maturityDate\":\"%10[^\"]\"", matExStr);
				}
			}
			
			if (rateKey)  { char *colon = strchr(rateKey, ':'); if (colon) rate = strtod(colon + 1, NULL); }
			if (priceKey) { char *colon = strchr(priceKey, ':'); if (colon) price = strtod(colon + 1, NULL); }
			if (freqKey)  { char *colon = strchr(freqKey, ':'); if (colon) frequency = (int)strtol(colon + 1, NULL, 10); }

			// Evaluate, parse, and append data structures safely at block boundary
			if (strstr(line, "}") && strlen(typeStr) > 0 && strlen(matExStr) > 0) {
				
				int validInstrument = 0;
				if (strcmp(typeStr, "DEPOSIT") == 0) {
					instruments[instCount].type = DEPOSIT;
					validInstrument = 1;
				} else if (strcmp(typeStr, "FUTURE") == 0) {
					instruments[instCount].type = FUTURE;
					validInstrument = 1;
				} else if (strcmp(typeStr, "SWAP") == 0) {
					instruments[instCount].type = SWAP;
					validInstrument = 1;
				}

				if (validInstrument && instCount < maxInstruments) {
					DateTime startEvent = parseDateString(startExStr);
					DateTime matEvent = parseDateString(matExStr);

					instruments[instCount].startTime = calculateYearFraction(anchorDate, startEvent);
					instruments[instCount].maturity = calculateYearFraction(anchorDate, matEvent);
					instruments[instCount].rate = rate;
					instruments[instCount].price = price;
					instruments[instCount].paymentFrequency = frequency;
					instCount++;
				}

				// Clean buffers immediately so upcoming elements start from blank sheets
				typeStr[0] = '\0';
				startExStr[0] = '\0';
				matExStr[0] = '\0';
				rate = -1.0;
				price = -1.0;
				frequency = -1;
			}
		}
	}

	oisCurve->cbSchedule.numMeetings = cbCount;
	oisCurve->numNodes = oisCount;
	
	fclose(file);
	return instCount;
}


int loadDualCurvesFromJSON(const char *filename, InterestRateCurve *oisCurve, MarketInstrument instruments[], int maxInstruments) {
	FILE *file = fopen(filename, "r");
	if (!file) return -1;

	char line[256];
	int instCount = 0;
	int oisCount = 0;
	int readingOis = 0;

	char typeStr[32] = "";
	double startTime = 0.0, maturity = -1.0, rate = -1.0, price = -1.0;
	int frequency = -1;

	while (fgets(line, sizeof(line), file)) {
		if (strstr(line, "\"ois_curve\"")) { readingOis = 1; continue; }
		if (strstr(line, "\"market_data\"")) { readingOis = 0; continue; }

		if (readingOis) {
			char *timeKey = strstr(line, "\"time\"");
			char *rateKey = strstr(line, "\"rate\"");
			if (timeKey) {
				char *colon = strchr(timeKey, ':');
				if (colon) oisCurve->times[oisCount] = strtod(colon + 1, NULL);
			}
			if (rateKey) {
				char *colon = strchr(rateKey, ':');
				if (colon) {
					double r = strtod(colon + 1, NULL);
					oisCurve->rates[oisCount] = r;
					oisCurve->dfs[oisCount] = exp(-r * oisCurve->times[oisCount]);
				}
			}
			if (strstr(line, "}") && oisCount < MAX_NODES) {
				oisCount++;
			}
		} else {
			char *typeKey = strstr(line, "\"type\"");
			char *startKey = strstr(line, "\"startTime\"");
			char *matKey = strstr(line, "\"maturity\"");
			char *rateKey = strstr(line, "\"rate\"");
			char *priceKey = strstr(line, "\"price\"");
			char *freqKey = strstr(line, "\"paymentFrequency\"");

			if (typeKey) {
				char *quote = strchr(typeKey + 6, '"');
				if (quote) {
					char *endQuote = strchr(quote + 1, '"');
					if (endQuote) {
						ptrdiff_t len = endQuote - (quote + 1);
						if (len > 31) len = 31;
						strncpy(typeStr, quote + 1, len);
						typeStr[len] = '\0';
					}
				}
			}
			if (startKey) { char *colon = strchr(startKey, ':'); if (colon) startTime = strtod(colon + 1, NULL); }
			if (matKey)   { char *colon = strchr(matKey, ':'); if (colon) maturity = strtod(colon + 1, NULL); }
			if (rateKey)  { char *colon = strchr(rateKey, ':'); if (colon) rate = strtod(colon + 1, NULL); }
			if (priceKey) { char *colon = strchr(priceKey, ':'); if (colon) price = strtod(colon + 1, NULL); }
			if (freqKey)  { char *colon = strchr(freqKey, ':'); if (colon) frequency = (int)strtol(colon + 1, NULL, 10); }

			if (strstr(line, "}") && maturity > 0.0 && instCount < maxInstruments) {
				if (strcmp(typeStr, "DEPOSIT") == 0) instruments[instCount].type = DEPOSIT;
				else if (strcmp(typeStr, "FUTURE") == 0) instruments[instCount].type = FUTURE;
				else if (strcmp(typeStr, "SWAP") == 0) instruments[instCount].type = SWAP;
				else { maturity = -1.0; continue; }

				instruments[instCount].startTime = startTime;
				instruments[instCount].maturity = maturity;
				instruments[instCount].rate = rate;
				instruments[instCount].price = price;
				instruments[instCount].paymentFrequency = frequency;
				instCount++;

				typeStr[0] = '\0'; startTime = 0.0; maturity = -1.0; rate = -1.0; price = -1.0; frequency = -1;
			}
		}
	}
	oisCurve->numNodes = oisCount;
	fclose(file);
	return instCount;
}

double computeImpliedForwardRate(InterestRateCurve* fwdCurve, double tStart, double tEnd, double dt) {
	if (dt <= 0.0) return 0.0;
	double dfStart = getDiscountFactor(fwdCurve, tStart);
	double dfEnd = getDiscountFactor(fwdCurve, tEnd);
	return (dfStart / dfEnd - 1.0) / dt;
}

double calculateLegPV(SwapLeg* leg, InterestRateCurve* fwdCurve, InterestRateCurve* oisCurve) {
	double legPV = 0.0;
	
	for (int i = 0; i < leg->numPeriods; i++) {
		SwapCashFlow cf = leg->periods[i];
		double dfDiscount = getDiscountFactor(oisCurve, cf.paymentTime);
		double rate = 0.0;
		
		if (leg->isFixed) {
			rate = cf.fixedRate;
		} else {
			rate = computeImpliedForwardRate(fwdCurve, cf.startTime, cf.endTime, cf.accrualFraction) + cf.spread;
		}
		
		legPV += cf.notional * rate * cf.accrualFraction * dfDiscount;
	}
	
	return legPV;
}

double calculateSwapNPV(VanillaSwap* swap, InterestRateCurve* fwdCurve, InterestRateCurve* oisCurve) {
	double pvFixed = calculateLegPV(&(swap->fixedLeg), fwdCurve, oisCurve);
	double pvFloating = calculateLegPV(&(swap->floatingLeg), fwdCurve, oisCurve);
	return pvFloating - pvFixed;
}

double calculateSwapDV01(VanillaSwap* swap, MarketInstrument marketInstruments[], int numInstruments, 
						 InterestRateCurve* oisCurve, double bpBumpSize) {
	
	InterestRateCurve fwdCurveBase;
	bootstrapCurve(&fwdCurveBase, oisCurve, marketInstruments, numInstruments);
	double baseNPV = calculateSwapNPV(swap, &fwdCurveBase, oisCurve);
	
	MarketInstrument* bumpedInstruments = malloc(numInstruments * sizeof(MarketInstrument));
	for (int i = 0; i < numInstruments; i++) {
		bumpedInstruments[i] = marketInstruments[i];
		if (bumpedInstruments[i].type == FUTURE) {
			bumpedInstruments[i].price -= (bpBumpSize * 100.0);
		} else {
			bumpedInstruments[i].rate += bpBumpSize;
		}
	}
	
	InterestRateCurve fwdCurveBumped;
	bootstrapCurve(&fwdCurveBumped, oisCurve, bumpedInstruments, numInstruments);
	
	double bumpedNPV = calculateSwapNPV(swap, &fwdCurveBumped, oisCurve);
	free(bumpedInstruments);
	
	return (bumpedNPV - baseNPV);
}

double solveForwardParSwapRate(InterestRateCurve *fwdCurve, InterestRateCurve *oisCurve, 
							   double forwardStart, double swapTenor, int frequency) {
	double dt = 1.0 / (double)frequency;
	int totalPeriods = (int)round(swapTenor * frequency);
	
	double floatingLegPV = 0.0;
	double fixedLegAnnuity = 0.0;
	
	for (int p = 1; p <= totalPeriods; p++) {
		double t_pay = forwardStart + (p * dt);
		double t_start = t_pay - dt;
		
		// 1. Discount factor from the OIS risk-free curve
		double df_ois = getDiscountFactor(oisCurve, t_pay);
		
		// 2. Projected implied forward rate from index curve
		double df_fwd_start = getDiscountFactor(fwdCurve, t_start);
		double df_fwd_end   = getDiscountFactor(fwdCurve, t_pay);
		double impliedFwd   = (df_fwd_start / df_fwd_end - 1.0) / dt;
		
		// Accumulate Legs
		floatingLegPV += impliedFwd * dt * df_ois;
		fixedLegAnnuity += dt * df_ois;
	}
	
	if (fixedLegAnnuity <= 0.0) return 0.0;
	return floatingLegPV / fixedLegAnnuity;
}

// ==========================================
// 8. Runtime Entry Point
// ==========================================

int main(int argc, char **argv) {
	const char *inputFile = "in_files/market_data_expanded.json";
	const char *curveAnchorToday = "2026-05-19";
	
	if (argc > 1) inputFile = argv[1];

	MarketInstrument marketData[MAX_NODES];
	InterestRateCurve oisCurve;
	InterestRateCurve fwdCurve;
	
	// 1. Initialize structural memory maps to zero clear garbage variables
	memset(&oisCurve, 0, sizeof(InterestRateCurve));
	memset(&fwdCurve, 0, sizeof(InterestRateCurve));

	// 2. Parse structural token data out of the JSON file
	int numInstruments = loadInstrumentsFromDatesJSON(inputFile, curveAnchorToday, &oisCurve, marketData, MAX_NODES);
		printf("%d", numInstruments);
	if (numInstruments <= 0) {
		printf("Fatal Calibration Error: Empty file maps or parsing violation.\n");
		return -1;
	}
// ==========================================
// Hardcoded Debug Initialization Block
// ==========================================
/*
int numInstruments = 13;

// Define Anchor/Today date string based on the data front-end
const char* anchorDateStr = "2026-05-20";
DateTime anchorDate = parseDateString(anchorDateStr);

// 1. Initialize and populate oisCurve structures
//InterestRateCurve oisCurve;
//memset(&oisCurve, 0, sizeof(InterestRateCurve));

// --- Central Bank Meeting Schedules ---
oisCurve.cbSchedule.numMeetings = 9;
const char* meetingDates[] = {
	"2026-05-20", "2026-05-28", "2026-07-09", "2026-09-03", "2026-10-29", 
	"2026-12-10", "2027-02-11", "2027-03-18", "2027-05-06"
};
double targetRates[] = {
	0.022504, 0.023129, 0.024876, 0.027054, 0.028675, 
	0.030829, 0.032578, 0.034205, 0.035198
};
for (int i = 0; i < 9; i++) {
	DateTime mtDate = parseDateString(meetingDates[i]);
	oisCurve.cbSchedule.meetingTimes[i] = calculateYearFraction(anchorDate, mtDate);
	oisCurve.cbSchedule.targetRates[i] = targetRates[i];
}

// --- Static OIS Term Curve Nodes ---
oisCurve.numNodes = 8;
const char* oisDates[] = {
	"2028-05-19", "2029-05-19", "2030-05-19", "2031-05-19", 
	"2033-05-19", "2036-05-19", "2038-05-19", "2041-05-19"
};
double oisRates[] = {
	0.03405, 0.0363, 0.0378, 0.0389, 0.04083, 0.04294, 0.04397, 0.04545
};
for (int i = 0; i < 8; i++) {
	DateTime targetOisDate = parseDateString(oisDates[i]);
	double t = calculateYearFraction(anchorDate, targetOisDate);
	oisCurve.times[i] = t;
	oisCurve.rates[i] = oisRates[i];
	oisCurve.dfs[i] = exp(-oisRates[i] * t);
}


// 2. Initialize and populate market instruments array
#define NUM_MARKET_INSTRUMENTS 13
//MarketInstrument marketData[NUM_MARKET_INSTRUMENTS];
//memset(marketData, 0, sizeof(marketData));

// Struct representation of your raw dataset row configurations
struct RawInput {
	InstrumentType type;
	const char* start;
	const char* maturity;
	double rate;
	double price;
	int frequency;
};

struct RawInput dataset[NUM_MARKET_INSTRUMENTS] = {
	{ DEPOSIT, "2026-05-20", "2026-08-24", 0.02645,   0.0,   12 },
	{ FUTURE,  "2026-06-10", "2026-09-16", 0.0,	   97.30,  4 },
	{ FUTURE,  "2026-09-16", "2026-12-16", 0.0,	   96.92,  4 },
	{ FUTURE,  "2026-12-16", "2027-03-10", 0.0,	   96.54,  4 },
	{ FUTURE,  "2027-03-17", "2027-06-16", 0.0,	   96.24,  4 },
	{ SWAP,	"2026-05-22", "2028-05-22", 0.036375,  0.0,	2 },
	{ SWAP,	"2026-05-22", "2029-05-22", 0.038425,  0.0,	2 },
	{ SWAP,	"2026-05-22", "2030-05-22", 0.0398,	0.0,	2 },
	{ SWAP,	"2026-05-22", "2031-05-22", 0.04085,   0.0,	2 },
	{ SWAP,	"2026-05-22", "2033-05-23", 0.0426,	0.0,	2 },
	{ SWAP,	"2026-05-22", "2036-05-22", 0.044575,  0.0,	2 },
	{ SWAP,	"2026-05-22", "2038-05-24", 0.045575,  0.0,	2 },
	{ SWAP,	"2026-05-22", "2041-05-22", 0.046925,  0.0,	2 }
};

// Loop through the layout vector to populate linear Act/365 coordinates
for (int i = 0; i < NUM_MARKET_INSTRUMENTS; i++) {
	DateTime startEvent = parseDateString(dataset[i].start);
	DateTime matEvent = parseDateString(dataset[i].maturity);
	
	marketData[i].type = dataset[i].type;
	marketData[i].startTime = calculateYearFraction(anchorDate, startEvent);
	marketData[i].maturity = calculateYearFraction(anchorDate, matEvent);
	marketData[i].rate = dataset[i].rate;
	marketData[i].price = dataset[i].price;
	marketData[i].paymentFrequency = dataset[i].frequency;
}
*/

	printf("========================================================================\n");
	printf("   DUAL-CURVE CONFIGURATION ENGAGED (Anchor Pricing Date: %s)\n", curveAnchorToday);
	printf("========================================================================\n");
	printf("-> Parsed Central Bank Meetings Matrix: %d Schedule Nodes found.\n", oisCurve.cbSchedule.numMeetings);
	printf("-> Parsed Standard OIS Curve Tail Nodes: %d Discount Nodes found.\n", oisCurve.numNodes);
	//printf("-> Parsed Projection Curve Market Instruments: %d Assets found.\n", numInstruments);

	// Set an anchor starting point rate for our OIS curve (Today's effective fed fund rate or ESTR)
	oisCurve.rates[0] = 0.022500;

	// 3. Define the interpolation regime stack for the OIS Discount Curve
	// Find the exact timeline point where our central bank schedule steps stop
	double finalMeetingTime = oisCurve.cbSchedule.meetingTimes[oisCurve.cbSchedule.numMeetings - 1];
	
	oisCurve.regimes[0].upper_time_boundary = finalMeetingTime;
	oisCurve.regimes[0].interp_func = NULL;// Flag handling inside getDiscountFactor uses step integration
	oisCurve.regimes[1].upper_time_boundary = 100.0;
	oisCurve.regimes[1].interp_func = interpolateLogLinearDf;// Long end transitions to log-linear
	oisCurve.numRegimes = 2;

	// 4. Trigger the Forward Projection Curve bootstrap matrix calculation
	printf("\nBootstrapping Forward Projection Curve via Multi-Regime Spline Engine...\n");
	bootstrapCurve(&fwdCurve, &oisCurve, marketData, numInstruments);
	printf("Recalibration Complete. Forward Projection Curve nodes stabilized.\n");

	// 5. Downstream Analytics: Output Calibrated Vectors
	printf("\n--- Calibrated Forward Curve Zero Vector Data ---\n");
	for (int i = 0; i < fwdCurve.numNodes; i++) {
		const char* typeLabel = (marketData[i].type == DEPOSIT) ? "DEPO" : 
								(marketData[i].type == FUTURE)  ? "ED-FUT" : "SWAP";
printf("Node %2d | Maturity Year Fraction: %5.2f Y | Instrument: %-6s | Solved Zero: %.4f%%\n", 
	   i, fwdCurve.times[i], typeLabel, fwdCurve.rates[i] * 100.0);
	}

	// 6. Execute Dual-Curve Par Swap Solver Evaluations
	printf("\n========================================================================\n");
	printf("   DOWNSTREAM OIS-DISCOUNTED PAR SWAP RATINGS (Dual Curve Execution)\n");
	printf("========================================================================\n");
	
	double targetTenors[] = { 2.00, 3.00, 5.00, 10.00, 15.00 };
	int targetFrequencies[] = { 2, 2, 2, 2, 2 };// Standard semi-annual payment legs
	int totalTenors = sizeof(targetTenors) / sizeof(targetTenors[0]);

	for (int i = 0; i < totalTenors; i++) {
		double parRate = solveParSwapRate(&fwdCurve, &oisCurve, targetTenors[i], targetFrequencies[i]);
		
		// Find matching input baseline rate for structural spread observation
		double originalMarketRate = 0.0;
		for (int m = 0;m < numInstruments;m++) {
			if (marketData[m].type == SWAP && fabs(marketData[m].maturity - targetTenors[i]) < 1e-2) {
				originalMarketRate = marketData[m].rate;
				break;
			}
		}
		
		printf("Tenor: %2.0f-Year | Frequency: Semiannual | OIS-Discounted Par Rate = %.4f%% ", 
			   targetTenors[i], parRate * 100.0);
		if (originalMarketRate > 0.0) {
			printf("(Market Input Reference: %.4f%%)\n", originalMarketRate * 100.0);
		} else {
			printf("(Interpolated Grid Point)\n");
		}
	}
	printf("========================================================================\n");
// ========================================================================
	//	FORWARD STARTING OIS-DISCOUNTED PAR SWAP RATINGS
	// ========================================================================
	printf("\n========================================================================\n");
	printf("   FORWARD STARTING PAR SWAPS (Dual Curve Execution)\n");
	printf("========================================================================\n");

	// Define structural forward matrices: {Forward Start, Swap Tenor}
	double fwdStarts[] = { 1.0, 2.0, 5.0 };
	double fwdTenors[] = { 1.0, 1.0, 5.0 };
	const char* labels[] = { "1y1y", "2y1y", "5y5y" };
	int fwdFrequency = 2;// Semiannual payment legs

	int totalFwdInstruments = sizeof(fwdStarts) / sizeof(fwdStarts[0]);

	for (int i = 0; i < totalFwdInstruments; i++) {
		double fwdParRate = solveForwardParSwapRate(&fwdCurve, &oisCurve, 
												   fwdStarts[i], fwdTenors[i], 
												   fwdFrequency);
		printf("Structure: %-6s | Start: %3.1f Y | Tenor: %3.1f Y | Forward Par Rate = %.4f%%\n", 
			   labels[i], fwdStarts[i], fwdTenors[i], fwdParRate * 100.0);
	}
	printf("========================================================================\n");

	return 0;
}

// Add these at the bottom of your C file to expose them via the Shared Object

// Allocation helper for Python
MarketInstrument* create_instrument_pool(int size) {
	return (MarketInstrument*)calloc(size, sizeof(MarketInstrument));
}

// Wrapper function Python can call to trigger the dual-curve engine
void run_calibration_bridge(InterestRateCurve* fwdCurve, InterestRateCurve* oisCurve, 
							MarketInstrument* instruments, int numInstruments) {
	// Reset memory maps
	memset(fwdCurve, 0, sizeof(InterestRateCurve));
	
	// Set up default OIS interpolation boundaries
	oisCurve->regimes[0].upper_time_boundary = 100.0;
	oisCurve->regimes[0].interp_func = interpolateLogLinearDf;
	oisCurve->numRegimes = 1;

	// Run the engine
	bootstrapCurve(fwdCurve, oisCurve, instruments, numInstruments);
}

void free_instrument_pool(MarketInstrument* ptr) {
	free(ptr);
}

// Exposed C-Bridge target wrapper for Python ctypes mapping
double run_swap_analytics_bridge(
	double* fixedSchedule, double* floatingSchedule, int numPeriods, double fixedRate,
	InterestRateCurve* fwdCurve, InterestRateCurve* oisCurve, double* out_fixed_pv, double* out_float_pv) 
{
	// 1. Manually build the fixed and floating structures from the inbound Python arrays
	SwapCashFlow* fixedCFs = malloc(numPeriods * sizeof(SwapCashFlow));
	SwapCashFlow* floatCFs = malloc(numPeriods * sizeof(SwapCashFlow));
	
	for(int i = 0; i < numPeriods;i++) {
		double tStart = (i == 0) ? 0.0 : fixedSchedule[i-1];
		double tEnd = fixedSchedule[i];

		fixedCFs[i] = (SwapCashFlow){.startTime = tStart, .endTime = tEnd, .paymentTime = tEnd, 
									 .accrualFraction = (tEnd - tStart), .fixedRate = fixedRate, .notional = 10000000.0};

		floatCFs[i] = (SwapCashFlow){.startTime = tStart, .endTime = tEnd, .paymentTime = tEnd, 
									 .accrualFraction = (tEnd - tStart), .spread = 0.0, .notional = 10000000.0};
	}

	VanillaSwap swap = {
		.fixedLeg = {.periods = fixedCFs, .numPeriods = numPeriods, .isFixed = 1},
		.floatingLeg = {.periods = floatCFs, .numPeriods = numPeriods, .isFixed = 0}
	};

	// 2. Perform analytics
	*out_fixed_pv = calculateLegPV(&swap.fixedLeg, fwdCurve, oisCurve);
	*out_float_pv = calculateLegPV(&swap.floatingLeg, fwdCurve, oisCurve);
	double npv = calculateSwapNPV(&swap, fwdCurve, oisCurve);

	free(fixedCFs);
	free(floatCFs);

	return npv;
}
