
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>

#include "date_utils.h"
#include "dual_curve.h"

// ==========================================
// 2. Concrete Interpolation Engines
// ==========================================



/* ====================================================================
 * PART 1 – Log-DF Cubic Spline
 *
 * Motivation
 * ----------
 * The existing setupCubicSpline() fits a natural cubic spline to the
 * continuously-compounded zero rates r(t).  Converting back to discount
 * factors via exp(-r*t) can produce non-monotone DFs between nodes when
 * rates have significant curvature, because the composition of a cubic
 * in r with the t-multiplier is not itself monotone-preserving.
 *
 * Fitting the spline to y_i = log(DF_i) = -r_i * t_i instead works
 * directly in the space that determines prices.  The interpolated
 * quantity is the log-DF, which is monotone-decreasing by construction
 * (for positive rates), so exp(spline(t)) is guaranteed to be a valid
 * discount factor.
 *
 * Algorithm
 * ---------
 * Natural cubic spline (second derivative = 0 at both endpoints) solved
 * via the standard O(n) tridiagonal (Thomas) algorithm, identical to the
 * existing setupCubicSpline() but with y_i = log(DF_i).
 *
 * The four coefficient arrays spline_a/b/c/d already present on
 * InterestRateCurve are reused:
 *   spline_a[i] = y_i			(log-DF at node i)
 *   spline_b[i] = first  deriv at node i
 *   spline_c[i] = second deriv at node i / 2
 *   spline_d[i] = third  deriv at node i / 6
 *
 * Because these arrays are shared with setupCubicSpline() and
 * setupParabolicSpline(), only ONE setup function should be active
 * at a time.  bootstrapCurve() below calls setupLogDfCubicSpline()
 * and does NOT call the others.
 * ==================================================================== */
 
void setupLogDfCubicSpline(InterestRateCurve *curve)
{
	int n = curve->numNodes - 1;   /* number of intervals */
	if (n < 1) return;
 
	/* ----------------------------------------------------------------
	 * y_i = log( DF(t_i) ) = -r_i * t_i
	 * Special case: t_0 = 0  →  log(DF(0)) = log(1) = 0 exactly.
	 * ---------------------------------------------------------------- */
	double y[MAX_NODES];
	for (int i = 0; i <= n; i++) {
		double t = curve->times[i];
		double r = curve->rates[i];
		y[i] = (t > 0.0) ? (-r * t) : 0.0;
	}
 
	/* ----------------------------------------------------------------
	 * Interval widths
	 * ---------------------------------------------------------------- */
	double h[MAX_NODES];
	for (int i = 0; i < n; i++)
		h[i] = curve->times[i+1] - curve->times[i];
 
	/* ----------------------------------------------------------------
	 * Right-hand side of the tridiagonal system (natural spline:
	 * second derivatives at both endpoints forced to zero).
	 * ---------------------------------------------------------------- */
	double alpha[MAX_NODES];
	for (int i = 1; i < n; i++)
		alpha[i] = (3.0 / h[i])   * (y[i+1] - y[i]) -
				   (3.0 / h[i-1]) * (y[i]   - y[i-1]);
 
	/* ----------------------------------------------------------------
	 * Thomas algorithm (forward sweep)
	 * ---------------------------------------------------------------- */
	double l[MAX_NODES], mu[MAX_NODES], z[MAX_NODES];
 
	l[0]  = 1.0;
	mu[0] = 0.0;
	z[0]  = 0.0;
 
	for (int i = 1; i < n; i++) {
		l[i]  = 2.0 * (curve->times[i+1] - curve->times[i-1]) - h[i-1] * mu[i-1];
		mu[i] = h[i] / l[i];
		z[i]  = (alpha[i] - h[i-1] * z[i-1]) / l[i];
	}
 
	l[n] = 1.0;
	z[n] = 0.0;
 
	/* ----------------------------------------------------------------
	 * Back-substitution: solve for c (the second-derivative / 2 coeffs)
	 * ---------------------------------------------------------------- */
	double c[MAX_NODES];
	c[n] = 0.0;   /* natural spline endpoint condition */
	for (int j = n - 1; j >= 0; j--)
		c[j] = z[j] - mu[j] * c[j+1];
 
	/* ----------------------------------------------------------------
	 * Store all four Hermite coefficients.
	 * For segment [t_i, t_{i+1}], the spline value at t_i + dx is:
	 *
	 *   logDF(t) = a[i] + b[i]*dx + c[i]*dx^2 + d[i]*dx^3
	 *
	 * where dx = t - t_i.
	 * ---------------------------------------------------------------- */
	for (int i = 0; i < n; i++) {
		curve->spline_a[i] = y[i];
		curve->spline_b[i] = (y[i+1] - y[i]) / h[i]
							  - h[i] * (c[i+1] + 2.0 * c[i]) / 3.0;
		curve->spline_c[i] = c[i];
		curve->spline_d[i] = (c[i+1] - c[i]) / (3.0 * h[i]);
	}
 
	/* Final node: copy anchor value (needed for exact evaluation at t_n) */
	curve->spline_a[n] = y[n];
	curve->spline_b[n] = 0.0;
	curve->spline_c[n] = 0.0;
	curve->spline_d[n] = 0.0;
}
 
 
/* --------------------------------------------------------------------
 * interpolateLogDfCubic
 *
 * Evaluates the log-DF cubic spline at time t, returning DF(t).
 * idx is the left-bracket index such that times[idx] <= t < times[idx+1],
 * computed by the binary search already in getDiscountFactor().
 *
 * For t exactly at a node, dx = 0 and the result reduces to
 * exp(spline_a[idx]) = exp(log(DF_i)) = DF_i  ✓
 * -------------------------------------------------------------------- */
double interpolateLogDfCubic(InterestRateCurve *curve, double t, int idx)
{
	double dx	= t - curve->times[idx];
	double logDF = curve->spline_a[idx]
				 + curve->spline_b[idx] * dx
				 + curve->spline_c[idx] * dx * dx
				 + curve->spline_d[idx] * dx * dx * dx;
	return exp(logDF);
}


/* =========================================================
 * Monotone-Convex Interpolation  (Hagan & West 2006)
 * ---------------------------------------------------------
 * Interpolates in continuously-compounded zero-rate space.
 * Guarantees: (1) continuous forward rates, (2) positive
 * forwards wherever inputs are positive, (3) local – a node
 * change only affects adjacent segments.
 * ========================================================= */

/* Precompute discrete forward rates between knot points */
void setupMonotoneConvex(InterestRateCurve *curve)
{
	int n = curve->numNodes - 1;
	if (n < 1) return;
 
	/* Step 1: node log-DFs (Y_i = log DF_i = -r_i * t_i) */
	double Y[MAX_NODES];
	for (int i = 0; i <= n; i++)
		Y[i] = (curve->times[i] > 0.0) ? -curve->rates[i]*curve->times[i] : 0.0;
 
	/* Step 2: discrete secants delta_i = (Y_{i+1} - Y_i) / h_i
	 * Note: delta_i is NEGATIVE for positive rates (logDF decreases).
	 * The implied instantaneous forward at node i is -delta_i > 0.	*/
	double delta[MAX_NODES];
	double h[MAX_NODES];
	for (int i = 0; i < n; i++) {
		h[i]	 = curve->times[i+1] - curve->times[i];
		delta[i] = (Y[i+1] - Y[i]) / h[i];   /* negative for +ve rates */
	}
 
	/* Step 3: initial slopes m_i using harmonic mean of adjacent secants.
	 * This is the Hagan & West g_i formula recast in log-DF space.
	 * m_i = harmonic-mean weighted slope of log-DF at node i.
	 * For a monotone curve (all delta same sign), harmonic mean keeps
	 * slopes within the range of adjacent secants.					 */
	double m[MAX_NODES];
 
	/* Endpoint slopes: match the adjacent secant (natural choice) */
	m[0] = delta[0];
	m[n] = delta[n-1];
 
	for (int i = 1; i < n; i++) {
		/* If adjacent secants have opposite signs → local extremum.
		 * Set slope to zero (Fritsch-Carlson condition).			   */
		if (delta[i-1] * delta[i] <= 0.0) {
			m[i] = 0.0;
		} else {
			/* Weighted harmonic mean (Fritsch-Carlson eq. 5.2) */
			double w1 = 2.0*h[i]   + h[i-1];
			double w2 = h[i]   + 2.0*h[i-1];
			m[i] = (w1 + w2) / (w1/delta[i-1] + w2/delta[i]);
		}
	}
 
	/* Step 4: Fritsch-Carlson monotonicity conditions.
	 * For each segment i, compute alpha = m[i]/delta[i] and
	 * beta = m[i+1]/delta[i].  Monotonicity requires alpha^2 + beta^2 <= 9.
	 * If violated, scale both slopes down.							 */
	for (int i = 0; i < n; i++) {
		if (fabs(delta[i]) < 1e-15) {
			/* Flat segment: force both endpoint slopes to zero */
			m[i] = m[i+1] = 0.0;
			continue;
		}
		double alpha = m[i]   / delta[i];
		double beta  = m[i+1] / delta[i];
		double sq	= alpha*alpha + beta*beta;
		if (sq > 9.0) {
			double tau = 3.0 / sqrt(sq);
			m[i]   = tau * alpha * delta[i];
			m[i+1] = tau * beta  * delta[i];
		}
	}
 
	/* Step 5: cubic Hermite coefficients on each segment.
	 * Standard Hermite basis in local coord x = t - t_i:
	 *   p(0) = Y_i   → a = Y_i
	 *   p(h) = Y_{i+1}
	 *   p'(0) = m_i
	 *   p'(h) = m_{i+1}
	 *
	 *   b = m_i
	 *   c = (3*delta_i - 2*m_i - m_{i+1}) / h_i
	 *   d = (m_i + m_{i+1} - 2*delta_i) / h_i^2					  */
	for (int i = 0; i < n; i++) {
		double hi = h[i];
		curve->spline_a[i] = Y[i];
		curve->spline_b[i] = m[i];
		curve->spline_c[i] = (3.0*delta[i] - 2.0*m[i] - m[i+1]) / hi;
		curve->spline_d[i] = (m[i] + m[i+1] - 2.0*delta[i]) / (hi*hi);
	}
	curve->spline_a[n] = Y[n];
	curve->spline_b[n] = m[n];
	curve->spline_c[n] = 0.0;
	curve->spline_d[n] = 0.0;
}
 
/* ------------------------------------------------------------------ */
double interpolateMonotoneConvex(InterestRateCurve *curve, double t, int idx)
{
	double x	 = t - curve->times[idx];
	double logDF = curve->spline_a[idx]
				 + curve->spline_b[idx]*x
				 + curve->spline_c[idx]*x*x
				 + curve->spline_d[idx]*x*x*x;
	return exp(logDF);
}

double interpolateLogLinearDf(InterestRateCurve *curve, double t, int32_t idx) {
	double t0 = curve->times[idx], t1 = curve->times[idx+1];
	double df0 = curve->dfs[idx], df1 = curve->dfs[idx+1];
	if (df0 <= 0.0 || df1 <= 0.0) return 0.0;
	return exp(log(df0) + (t - t0) / (t1 - t0) * (log(df1) - log(df0)));
}

double interpolateStepWiseOIS(InterestRateCurve *curve, double t, int32_t idx) {
	int32_t numMeetings = curve->cbSchedule.numMeetings;
	
	// 1. If we are evaluating a time before the first meeting, 
	// use the curve's front anchor rate (Today's effective overnight rate)
	if (numMeetings == 0 || t < curve->cbSchedule.meetingTimes[0]) {
		return curve->rates[0];
	}
	
	// 2. Step through the schedule to find which meeting bracket time 't' falls into
	for (int32_t i = 0; i < numMeetings - 1; i++) {
		if (t >= curve->cbSchedule.meetingTimes[i] && t < curve->cbSchedule.meetingTimes[i+1]) {
			return curve->cbSchedule.targetRates[i];
		}
	}
	
	// 3. If 't' is past the last scheduled meeting, hold the final target rate flat
	return curve->cbSchedule.targetRates[numMeetings - 1];
}

double getStepWiseDiscountFactor(InterestRateCurve *curve, double t) {
	int32_t numMeetings = curve->cbSchedule.numMeetings;
	if (t <= 0.0) return 1.0;
	
	double integratedRateTime = 0.0;
	double tCurrent = 0.0;
	
	// Integrate across the steps up to time 't'
	for (int32_t i = 0; i < numMeetings; i++) {
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
	int32_t n = curve->numNodes - 1;
	if (n < 1) return;
	for (int32_t i = 0; i <= n; i++) curve->spline_a[i] = curve->rates[i];
	
	double initial_slope = (curve->rates[1] - curve->rates[0]) / (curve->times[1] - curve->times[0]);
	curve->spline_b[0] = initial_slope;

	for (int32_t i = 0; i < n; i++) {
		double h = curve->times[i+1] - curve->times[i];
		curve->spline_c[i] = (curve->rates[i+1] - curve->spline_a[i] - curve->spline_b[i] * h) / (h * h);
		if (i < n - 1) {
			curve->spline_b[i+1] = curve->spline_b[i] + 2.0 * curve->spline_c[i] * h;
		}
		curve->spline_d[i] = 0.0;
	}
}

double interpolateParabolicZero(InterestRateCurve *curve, double t, int32_t idx) {
	double dx = t - curve->times[idx];
	double r = curve->spline_a[idx] + curve->spline_b[idx] * dx + curve->spline_c[idx] * dx * dx;
	return exp(-r * t);
}

void setupCubicSpline(InterestRateCurve *curve) {
	int32_t n = curve->numNodes - 1;
	if (n < 2) return;
	double h[MAX_NODES], alpha[MAX_NODES], l[MAX_NODES], mu[MAX_NODES], z[MAX_NODES];

	for (int32_t i = 0; i <= n; i++) curve->spline_a[i] = curve->rates[i];
	for (int32_t i = 0; i < n; i++)  h[i] = curve->times[i+1] - curve->times[i];

	for (int32_t i = 1; i < n; i++) {
		alpha[i] = (3.0 / h[i]) * (curve->spline_a[i+1] - curve->spline_a[i]) - 
				   (3.0 / h[i-1]) * (curve->spline_a[i] - curve->spline_a[i-1]);
	}

	l[0] = 1.0;
	mu[0] = 0.0;
	z[0] = 0.0;
	l[n] = 1.0;
	z[n] = 0.0;
	curve->spline_c[n] = 0.0;

	for (int32_t i = 1; i < n; i++) {
		l[i] = 2.0 * (curve->times[i+1] - curve->times[i-1]) - h[i-1] * mu[i-1];
		mu[i] = h[i] / l[i];
		z[i] = (alpha[i] - h[i-1] * z[i-1]) / l[i];
	}

	for (int32_t j = n - 1; j >= 0; j--) {
		curve->spline_c[j] = z[j] - mu[j] * curve->spline_c[j+1];
		curve->spline_b[j] = (curve->spline_a[j+1] - curve->spline_a[j]) / h[j] - 
							 h[j] * (curve->spline_c[j+1] + 2.0 * curve->spline_c[j]) / 3.0;
		curve->spline_d[j] = (curve->spline_c[j+1] - curve->spline_c[j]) / (3.0 * h[j]);
	}
}

double interpolateCubicZero(InterestRateCurve *curve, double t, int32_t idx) {
	if (curve->numNodes < 3) return interpolateLogLinearDf(curve, t, idx);
	double dx = t - curve->times[idx];
	double r = curve->spline_a[idx] + curve->spline_b[idx] * dx +
			   curve->spline_c[idx] * dx * dx + curve->spline_d[idx] * dx * dx * dx;
	return exp(-r * t);
}


double getDiscountFactor(InterestRateCurve *curve, double t) {

	if (t <= 0.0) return 1.0;

	/* Stepwise OIS takes priority over everything for short-end times */
	if (curve->cbSchedule.numMeetings > 0) {
		double t_last = curve->cbSchedule.meetingTimes[curve->cbSchedule.numMeetings - 1];
		if (t <= t_last)
			return getStepWiseDiscountFactor(curve, t);
	}

	int32_t idx = 0;
	if (t >= curve->times[curve->numNodes - 1]) {
		idx = curve->numNodes - 2;
	} else {
		int32_t low = 0, high = curve->numNodes - 1;
		while (high - low > 1) {
			int32_t mid = (low + high) / 2;
			if (curve->times[mid] > t) high = mid;
			else low = mid;
		}
		idx = low;
	}

	for (int32_t i = 0; i < curve->numRegimes; i++) {
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
	
	int32_t expansions = 0;
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

	for (int32_t iter = 1; iter <= BRENT_MAX_ITER; iter++) {
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
	int32_t nodeIdx = ctx->currentNodeIdx;
	
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
		int32_t numPayments = (int)(inst->maturity * inst->paymentFrequency + 0.5);
		
		double tPrev = 0.0;
		for (int32_t i = 1; i <= numPayments; i++) {
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

double solveParSwapRate(InterestRateCurve *fwdCurve, InterestRateCurve *oisCurve, double maturity, int32_t frequency) 
{
	double dt = 1.0 / frequency;
	int32_t payments = (int)(maturity * frequency + 0.5);
	double floatingLeg = 0.0;
	double fixedAnnuity = 0.0;
	
	double tPrev = 0.0;
	for (int32_t i = 1; i <= payments; i++) {
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


void bootstrapCurve(InterestRateCurve *fwdCurve,
					InterestRateCurve *oisCurve,
					MarketInstrument   instruments[],
					int				numInstruments)
{
	/* ------------------------------------------------------------------
	 * Initialise the forward curve.
	 * Node 0 is always the anchor: t=0, DF=1, rate = first instrument's
	 * rate (used as the overnight/short-end anchor).
	 * ------------------------------------------------------------------ */
	fwdCurve->numNodes = 0;
	fwdCurve->times[0] = 0.0;
	fwdCurve->rates[0] = instruments[0].rate;
	fwdCurve->dfs[0]   = 1.0;
	fwdCurve->numNodes = 1;
 
	/* ------------------------------------------------------------------
	 * Main bootstrap loop – one node added per instrument.
	 * ------------------------------------------------------------------ */
	for (int i = 0; i < numInstruments; i++) {
		MarketInstrument *inst = &instruments[i];
		int idx = fwdCurve->numNodes;   /* index of the new node */
 
		/* ==============================================================
		 * DEPOSIT
		 * Simple add-on rate: DF = 1 / (1 + r * delta_t).
		 * Stored as continuously-compounded equivalent for consistency.
		 * ============================================================== */
		if (inst->type == DEPOSIT) {
			double t	  = inst->maturity;
			double delta  = t - inst->startTime;
			double df	 = 1.0 / (1.0 + inst->rate * delta);
 
			fwdCurve->times[idx] = t;
			fwdCurve->dfs[idx]   = df;
			fwdCurve->rates[idx] = (t > 0.0) ? (-log(df) / t) : inst->rate;
			fwdCurve->numNodes++;
 
			/* Rebuild spline after every node addition so that
			 * getDiscountFactor() remains consistent mid-bootstrap.	 */
//			setupLogDfCubicSpline(fwdCurve);
			setupMonotoneConvex(fwdCurve);
		}
 
		/* ==============================================================
		 * FUTURE
		 * Futures price → simple-rate implied forward → DF at t_end.
		 * Note: convexity adjustment can be applied here by subtracting
		 *   0.5 * sigma^2 * t_start * t_end  from impliedFwdRate before
		 *   computing df_end (see enhancement note #7 in the review).
		 * ============================================================== */
		else if (inst->type == FUTURE) {
			double t_start	   = inst->startTime;
			double t_end		 = inst->maturity;
			double delta_t	   = t_end - t_start;
			double impliedFwdRate = (100.0 - inst->price) / 100.0;
 
			double df_start = getDiscountFactor(fwdCurve, t_start);
			double df_end   = df_start / (1.0 + impliedFwdRate * delta_t);
 
			fwdCurve->times[idx] = t_end;
			fwdCurve->dfs[idx]   = df_end;
			fwdCurve->rates[idx] = (t_end > 0.0) ? (-log(df_end) / t_end) : 0.0;
			fwdCurve->numNodes++;
 
			setupMonotoneConvex(fwdCurve);
		}
 
		/* ==============================================================
		 * SWAP  –  Newton-Raphson bootstrap
		 *
		 * We seek the terminal zero rate z such that:
		 *
		 *   f(z) = PV_float(z) - PV_fixed = 0
		 *
		 * where PV_float uses the forward curve (including the new node
		 * at t_maturity with rate z) and PV_fixed uses the OIS discount
		 * curve.  f(z) is smooth and monotone in z, so N-R converges
		 * very quickly.
		 *
		 * Initial guess: the par swap rate is a good proxy for the
		 * terminal zero rate, especially for short tenors.  For long
		 * tenors (10Y+), using the previous node rate is also fine.
		 * ============================================================== */
		else if (inst->type == SWAP) {
			double t_maturity   = inst->maturity;
			double swapRate	 = inst->rate;
			int	freq		 = inst->paymentFrequency;
			int	totalPeriods = (int)round(t_maturity * freq);
			double dt		   = 1.0 / (double)freq;
 
			/* --- Helper: evaluate the NPV residual for a given trial rate --- */
			/* We define a local lambda-style inline via a macro to avoid
			 * repeating the loop body twice (for f and for f').			*/
 
#define EVAL_SWAP_NPV(trial_rate_, npv_out_)								   \
			do {															   \
				fwdCurve->times[idx] = t_maturity;							\
				fwdCurve->rates[idx] = (trial_rate_);						 \
				fwdCurve->dfs[idx]   = exp(-(trial_rate_) * t_maturity);	  \
				fwdCurve->numNodes   = idx + 1;							   \
				setupLogDfCubicSpline(fwdCurve);							  \
																			  \
				double _pvFloat = 0.0, _pvFixed = 0.0;						\
				double _tPrev   = 0.0;										\
				for (int _p = 1; _p <= totalPeriods; _p++) {				  \
					double _tPay	= _p * dt;								\
					double _dfDisc  = getDiscountFactor(oisCurve, _tPay);	 \
					double _dfFwdS  = getDiscountFactor(fwdCurve, _tPrev);	\
					double _dfFwdE  = getDiscountFactor(fwdCurve, _tPay);	 \
					double _fwdRate = (_dfFwdS / _dfFwdE - 1.0) / dt;		\
					_pvFloat += _fwdRate * dt * _dfDisc;					  \
					_pvFixed += swapRate  * dt * _dfDisc;					 \
					_tPrev	= _tPay;										\
				}															 \
				(npv_out_) = _pvFloat - _pvFixed;							 \
			} while (0)
 
			/* Initial guess: par swap rate is a good starting point.
			 * For the first swap node it's exact; for later ones the
			 * spline already gives a close starting DF.					*/
			double z = swapRate;
 
			/* Fallback guard: if the first guess is wildly out of range,
			 * nudge it toward something plausible.						 */
			if (z < -0.02) z = -0.02;
			if (z >  0.20) z =  0.20;
 
			double f = 0.0, f_pert = 0.0, fprime = 0.0, step = 0.0;
			int	converged = 0;
 
			for (int iter = 0; iter < NR_MAX_ITER; iter++) {
 
				/* Evaluate residual at current guess */
				EVAL_SWAP_NPV(z, f);
 
				if (fabs(f) < NR_TOLERANCE) {
					converged = 1;
					break;
				}
 
				/* Numerical derivative: perturb by NR_DERIV_EPS */
				EVAL_SWAP_NPV(z + NR_DERIV_EPS, f_pert);
				fprime = (f_pert - f) / NR_DERIV_EPS;
 
				/* Guard against near-zero derivative (flat objective) */
				if (fabs(fprime) < 1e-15) break;
 
				/* Newton step with damping: cap the step at 50bps per
				 * iteration to prevent overshooting in steep regions.	  */
				step = f / fprime;
				if (step >  0.005) step =  0.005;
				if (step < -0.005) step = -0.005;
				z -= step;
			}
 
			if (!converged) {
				/* If N-R hasn't converged (extremely unusual), fall back
				 * to bisection as a safety net.  Bracket is generous.	  */
				double lo = -0.05, hi = 0.50;
				double f_lo, f_hi, f_mid, mid;
				EVAL_SWAP_NPV(lo, f_lo);
				EVAL_SWAP_NPV(hi, f_hi);
 
				/* Expand bracket if needed */
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
					else					{ lo = mid; f_lo = f_mid; }
				}
				z = mid;
			}
 
#undef EVAL_SWAP_NPV
 
			/* Commit the solved node */
			fwdCurve->times[idx] = t_maturity;
			fwdCurve->rates[idx] = z;
			fwdCurve->dfs[idx]   = exp(-z * t_maturity);
			fwdCurve->numNodes   = idx + 1;
//			setupLogDfCubicSpline(fwdCurve);
			setupMonotoneConvex(fwdCurve);
		}
 
	} /* end instrument loop */
 
	/* ------------------------------------------------------------------
	 * Register interpolation regimes on the completed forward curve.
	 *
	 * Regime 0: stepwise OIS (short end, up to last CB meeting)
	 *   – only meaningful when this IS the OIS curve.  When fwdCurve is
	 *	 the IBOR projection curve, numMeetings will be 0 and this
	 *	 regime is skipped by getDiscountFactor().
	 *
	 * Regime 1: log-linear on DFs  [CB boundary → 2Y]
	 *   – fast and numerically stable; adequate where futures pin the
	 *	 curve tightly and curvature is low.
	 *
	 * Regime 2: log-DF cubic spline  [2Y → ∞]
	 *   – smooth, monotone DFs; correct instantaneous forward rates;
	 *	 preferred for swap pricing in the 2-30Y range.
	 * ------------------------------------------------------------------ */
	fwdCurve->numRegimes = 0;

	/* Determine the upper boundary of the stepwise OIS zone */
	double cb_boundary = 0.0;
	if (fwdCurve->cbSchedule.numMeetings > 0)
		cb_boundary = fwdCurve->cbSchedule.meetingTimes[
						  fwdCurve->cbSchedule.numMeetings - 1];

	/* Regime 0 – stepwise (only registered if CB schedule is populated) */
	if (cb_boundary > 0.0) {
		fwdCurve->regimes[0].upper_time_boundary = cb_boundary;
		fwdCurve->regimes[0].interp_func		 = interpolateStepWiseOIS;
		fwdCurve->numRegimes = 1;
	}

	/* Regime 1 – log-linear DF, short-to-medium end */
	fwdCurve->regimes[fwdCurve->numRegimes].upper_time_boundary = LOGDF_REGIME_BOUNDARY;
	fwdCurve->regimes[fwdCurve->numRegimes].interp_func		 = interpolateMonotoneConvex;
	fwdCurve->regimes[fwdCurve->numRegimes].interp_func		 = interpolateLogLinearDf;
	fwdCurve->numRegimes++;

	/* Regime 2 – log-DF cubic spline, long end (no upper boundary needed;
	 * getDiscountFactor() uses the last regime as a catch-all)			*/
	fwdCurve->regimes[fwdCurve->numRegimes].upper_time_boundary = 1e9;
	fwdCurve->regimes[fwdCurve->numRegimes].interp_func		 = interpolateLogDfCubic;
	fwdCurve->numRegimes++;
}

// ==========================================
// 7. Streaming Dual Curve JSON Tokenizer
// ==========================================


double computeImpliedForwardRate(InterestRateCurve* fwdCurve, double tStart, double tEnd, double dt) {
	if (dt <= 0.0) return 0.0;
	double dfStart = getDiscountFactor(fwdCurve, tStart);
	double dfEnd = getDiscountFactor(fwdCurve, tEnd);
	return (dfStart / dfEnd - 1.0) / dt;
}

double calculateLegPV(SwapLeg* leg, InterestRateCurve* fwdCurve, InterestRateCurve* oisCurve) {
	double legPV = 0.0;
	
	for (int32_t i = 0; i < leg->numPeriods; i++) {
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


double calculateSwapDV01(VanillaSwap *swap, MarketInstrument marketInstruments[],
						  int numInstruments, InterestRateCurve *oisCurve,
						  double bpBumpSize)
{
	/* Up bump */
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
	bootstrapCurve(&fwdUp, oisCurve, upInst, numInstruments);
	bootstrapCurve(&fwdDn, oisCurve, dnInst, numInstruments);
	double npvUp = calculateSwapNPV(swap, &fwdUp, oisCurve);
	double npvDn = calculateSwapNPV(swap, &fwdDn, oisCurve);
	free(upInst); free(dnInst);
	return (npvUp - npvDn) / 2.0;   /* central difference */
}

double solveForwardParSwapRate(InterestRateCurve *fwdCurve, InterestRateCurve *oisCurve, 
							   double forwardStart, double swapTenor, int32_t frequency) {
	double dt = 1.0 / (double)frequency;
	int32_t totalPeriods = (int)round(swapTenor * frequency);
	
	double floatingLegPV = 0.0;
	double fixedLegAnnuity = 0.0;
	
	for (int32_t p = 1; p <= totalPeriods; p++) {
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

int32_t main(int32_t argc, char **argv)
{

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
	int32_t numInstruments = loadInstrumentsFromDatesJSON(inputFile, curveAnchorToday, &oisCurve, marketData, MAX_NODES);
		printf("%d", numInstruments);
	if (numInstruments <= 0) {
		printf("Fatal Calibration Error: Empty file maps or parsing violation.\n");
		return -1;
	}

	printf("========================================================================\n");
	printf("   DUAL-CURVE CONFIGURATION ENGAGED (Anchor Pricing Date: %s)\n", curveAnchorToday);
	printf("========================================================================\n");
	printf("-> Parsed Central Bank Meetings Matrix: %d Schedule Nodes found.\n", oisCurve.cbSchedule.numMeetings);
	printf("-> Parsed Standard OIS Curve Tail Nodes: %d Discount Nodes found.\n", oisCurve.numNodes);
	//printf("-> Parsed Projection Curve Market Instruments: %d Assets found.\n", numInstruments);

	// Set an anchor starting point32_t rate for our OIS curve (Today's effective fed fund rate or ESTR)
	oisCurve.rates[0] = 0.022500;

	// 3. Define the interpolation regime stack for the OIS Discount Curve
	// Find the exact timeline point32_t where our central bank schedule steps stop
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
	for (int32_t i = 0; i < fwdCurve.numNodes; i++) {
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
	int32_t targetFrequencies[] = { 2, 2, 2, 2, 2 };// Standard semi-annual payment legs
	int32_t totalTenors = sizeof(targetTenors) / sizeof(targetTenors[0]);

	for (int32_t i = 0; i < totalTenors; i++) {
		double parRate = solveParSwapRate(&fwdCurve, &oisCurve, targetTenors[i], targetFrequencies[i]);
		
		// Find matching input baseline rate for structural spread observation
		double originalMarketRate = 0.0;
		for (int32_t m = 0;m < numInstruments;m++) {
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
	int32_t fwdFrequency = 2;// Semiannual payment legs

	int32_t totalFwdInstruments = sizeof(fwdStarts) / sizeof(fwdStarts[0]);

	for (int32_t i = 0; i < totalFwdInstruments; i++) {
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
MarketInstrument* create_instrument_pool(int32_t size) {
	return (MarketInstrument*)calloc(size, sizeof(MarketInstrument));
}

// Wrapper function Python can call to trigger the dual-curve engine
void run_calibration_bridge(InterestRateCurve* fwdCurve, InterestRateCurve* oisCurve, 
							MarketInstrument* instruments, int32_t numInstruments) {
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
	double* fixedSchedule, double* floatingSchedule, int32_t numPeriods, double fixedRate,
	InterestRateCurve* fwdCurve, InterestRateCurve* oisCurve, double* out_fixed_pv, double* out_float_pv) 
{
	// 1. Manually build the fixed and floating structures from the inbound Python arrays
	SwapCashFlow* fixedCFs = malloc(numPeriods * sizeof(SwapCashFlow));
	SwapCashFlow* floatCFs = malloc(numPeriods * sizeof(SwapCashFlow));
	
	for(int32_t i = 0; i < numPeriods;i++) {
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

// ==========================================
// Hardcoded Debug Initialization Block
// ==========================================
/*
int32_t numInstruments = 13;

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
for (int32_t i = 0; i < 9; i++) {
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
for (int32_t i = 0; i < 8; i++) {
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
	int32_t frequency;
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
for (int32_t i = 0; i < NUM_MARKET_INSTRUMENTS; i++) {
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
