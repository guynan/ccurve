#include <math.h>
#include <stdint.h>

#include "dual_curve.h"

/* ====================================================================
 * Log-DF Cubic Spline
 *
 * Fits a natural cubic spline to y_i = log(DF_i) = -r_i * t_i.
 * Interpolating in log-DF space guarantees monotone discount factors.
 *
 * Coefficients stored in spline_a/b/c/d (shared with other setups;
 * only one setup function should be active at a time):
 *   a[i] = log(DF_i)         (value at node)
 *   b[i] = first derivative
 *   c[i] = second deriv / 2
 *   d[i] = third deriv / 6
 * ==================================================================== */
void setupLogDfCubicSpline(InterestRateCurve *curve)
{
    int n = curve->numNodes - 1;
    if (n < 1) return;

    double y[MAX_NODES];
    for (int i = 0; i <= n; i++) {
        double t = curve->times[i];
        double r = curve->rates[i];
        y[i] = (t > 0.0) ? (-r * t) : 0.0;
    }

    double h[MAX_NODES];
    for (int i = 0; i < n; i++)
        h[i] = curve->times[i+1] - curve->times[i];

    double alpha[MAX_NODES];
    for (int i = 1; i < n; i++)
        alpha[i] = (3.0 / h[i])   * (y[i+1] - y[i]) -
                   (3.0 / h[i-1]) * (y[i]   - y[i-1]);

    /* Thomas algorithm (forward sweep) */
    double l[MAX_NODES], mu[MAX_NODES], z[MAX_NODES];
    l[0] = 1.0; mu[0] = 0.0; z[0] = 0.0;

    for (int i = 1; i < n; i++) {
        l[i]  = 2.0 * (curve->times[i+1] - curve->times[i-1]) - h[i-1] * mu[i-1];
        mu[i] = h[i] / l[i];
        z[i]  = (alpha[i] - h[i-1] * z[i-1]) / l[i];
    }
    l[n] = 1.0; z[n] = 0.0;

    /* Back-substitution */
    double c[MAX_NODES];
    c[n] = 0.0;
    for (int j = n - 1; j >= 0; j--)
        c[j] = z[j] - mu[j] * c[j+1];

    for (int i = 0; i < n; i++) {
        curve->spline_a[i] = y[i];
        curve->spline_b[i] = (y[i+1] - y[i]) / h[i]
                              - h[i] * (c[i+1] + 2.0 * c[i]) / 3.0;
        curve->spline_c[i] = c[i];
        curve->spline_d[i] = (c[i+1] - c[i]) / (3.0 * h[i]);
    }
    curve->spline_a[n] = y[n];
    curve->spline_b[n] = 0.0;
    curve->spline_c[n] = 0.0;
    curve->spline_d[n] = 0.0;
}

double interpolateLogDfCubic(const InterestRateCurve *curve, double t, int32_t idx)
{
    double dx    = t - curve->times[idx];
    double logDF = curve->spline_a[idx]
                 + curve->spline_b[idx] * dx
                 + curve->spline_c[idx] * dx * dx
                 + curve->spline_d[idx] * dx * dx * dx;
    return exp(logDF);
}


/* ====================================================================
 * Monotone-Convex Interpolation  (Hagan & West 2006)
 *
 * Interpolates in log-DF space using cubic Hermite polynomials with
 * slopes chosen via a harmonic-mean weighting (Fritsch-Carlson).
 * Guarantees: continuous forward rates, positive forwards when inputs
 * are positive, and locality (node change affects only neighbours).
 * ==================================================================== */
void setupMonotoneConvex(InterestRateCurve *curve)
{
    int n = curve->numNodes - 1;
    if (n < 1) return;

    double Y[MAX_NODES];
    for (int i = 0; i <= n; i++)
        Y[i] = (curve->times[i] > 0.0) ? -curve->rates[i] * curve->times[i] : 0.0;

    double delta[MAX_NODES], h[MAX_NODES];
    for (int i = 0; i < n; i++) {
        h[i]     = curve->times[i+1] - curve->times[i];
        delta[i] = (Y[i+1] - Y[i]) / h[i];
    }

    double m[MAX_NODES];
    m[0] = delta[0];
    m[n] = delta[n-1];

    for (int i = 1; i < n; i++) {
        if (delta[i-1] * delta[i] <= 0.0) {
            m[i] = 0.0;
        } else {
            double w1 = 2.0 * h[i]   + h[i-1];
            double w2 =       h[i]   + 2.0 * h[i-1];
            m[i] = (w1 + w2) / (w1 / delta[i-1] + w2 / delta[i]);
        }
    }

    for (int i = 0; i < n; i++) {
        if (fabs(delta[i]) < 1e-15) {
            m[i] = m[i+1] = 0.0;
            continue;
        }
        double alpha = m[i]   / delta[i];
        double beta  = m[i+1] / delta[i];
        double sq    = alpha * alpha + beta * beta;
        if (sq > 9.0) {
            double tau = 3.0 / sqrt(sq);
            m[i]   = tau * alpha * delta[i];
            m[i+1] = tau * beta  * delta[i];
        }
    }

    for (int i = 0; i < n; i++) {
        double hi = h[i];
        curve->spline_a[i] = Y[i];
        curve->spline_b[i] = m[i];
        curve->spline_c[i] = (3.0 * delta[i] - 2.0 * m[i] - m[i+1]) / hi;
        curve->spline_d[i] = (m[i] + m[i+1] - 2.0 * delta[i]) / (hi * hi);
    }
    curve->spline_a[n] = Y[n];
    curve->spline_b[n] = m[n];
    curve->spline_c[n] = 0.0;
    curve->spline_d[n] = 0.0;
}

double interpolateMonotoneConvex(const InterestRateCurve *curve, double t, int32_t idx)
{
    double x     = t - curve->times[idx];
    double logDF = curve->spline_a[idx]
                 + curve->spline_b[idx] * x
                 + curve->spline_c[idx] * x * x
                 + curve->spline_d[idx] * x * x * x;
    return exp(logDF);
}


/* --- Log-linear on discount factors --------------------------------- */
double interpolateLogLinearDf(const InterestRateCurve *curve, double t, int32_t idx)
{
    double t0  = curve->times[idx],   t1  = curve->times[idx+1];
    double df0 = curve->dfs[idx],     df1 = curve->dfs[idx+1];
    if (df0 <= 0.0 || df1 <= 0.0) return 0.0;
    return exp(log(df0) + (t - t0) / (t1 - t0) * (log(df1) - log(df0)));
}


/* ====================================================================
 * Stepwise OIS
 *
 * interpolateStepWiseOIS: returns the step-function rate active at t.
 *   (Diagnostic / forward-rate use; does NOT return a DF.)
 *
 * getStepWiseDiscountFactor: integrates the piecewise-constant rate
 *   across CB meeting boundaries to return DF(0, t) = exp(-∫r(s)ds).
 *
 * interpolateStepWiseDF: thin wrapper matching InterpolationFunction
 *   so the stepwise method can be registered as a regime.
 * ==================================================================== */
double interpolateStepWiseOIS(const InterestRateCurve *curve, double t, int32_t idx)
{
    (void)idx;
    int32_t numMeetings = curve->cbSchedule.numMeetings;

    if (numMeetings == 0 || t < curve->cbSchedule.meetingTimes[0])
        return curve->rates[0];

    for (int32_t i = 0; i < numMeetings - 1; i++) {
        if (t >= curve->cbSchedule.meetingTimes[i] &&
            t <  curve->cbSchedule.meetingTimes[i+1])
            return curve->cbSchedule.targetRates[i];
    }
    return curve->cbSchedule.targetRates[numMeetings - 1];
}

double getStepWiseDiscountFactor(const InterestRateCurve *curve, double t)
{
    int32_t numMeetings = curve->cbSchedule.numMeetings;
    if (t <= 0.0) return 1.0;

    double intRateTime = 0.0;
    double tCurrent    = 0.0;

    for (int32_t i = 0; i < numMeetings; i++) {
        double tMeeting  = curve->cbSchedule.meetingTimes[i];
        double activeRate = (i == 0) ? curve->rates[0]
                                     : curve->cbSchedule.targetRates[i-1];

        if (t <= tMeeting) {
            intRateTime += activeRate * (t - tCurrent);
            tCurrent = t;
            break;
        } else {
            intRateTime += activeRate * (tMeeting - tCurrent);
            tCurrent = tMeeting;
        }
    }

    if (t > tCurrent) {
        double finalRate = curve->cbSchedule.targetRates[numMeetings - 1];
        intRateTime += finalRate * (t - tCurrent);
    }

    return exp(-intRateTime);
}

double interpolateStepWiseDF(const InterestRateCurve *curve, double t, int32_t idx)
{
    (void)idx;
    return getStepWiseDiscountFactor(curve, t);
}


/* --- Parabolic zero-rate spline ------------------------------------- */
void setupParabolicSpline(InterestRateCurve *curve)
{
    int32_t n = curve->numNodes - 1;
    if (n < 1) return;

    for (int32_t i = 0; i <= n; i++) curve->spline_a[i] = curve->rates[i];

    double initial_slope = (curve->rates[1] - curve->rates[0]) /
                           (curve->times[1]  - curve->times[0]);
    curve->spline_b[0] = initial_slope;

    for (int32_t i = 0; i < n; i++) {
        double h = curve->times[i+1] - curve->times[i];
        curve->spline_c[i] = (curve->rates[i+1] - curve->spline_a[i] -
                               curve->spline_b[i] * h) / (h * h);
        if (i < n - 1)
            curve->spline_b[i+1] = curve->spline_b[i] + 2.0 * curve->spline_c[i] * h;
        curve->spline_d[i] = 0.0;
    }
}

double interpolateParabolicZero(const InterestRateCurve *curve, double t, int32_t idx)
{
    double dx = t - curve->times[idx];
    double r  = curve->spline_a[idx]
              + curve->spline_b[idx] * dx
              + curve->spline_c[idx] * dx * dx;
    return exp(-r * t);
}


/* --- Natural cubic spline on zero rates ----------------------------- */
void setupCubicSpline(InterestRateCurve *curve)
{
    int32_t n = curve->numNodes - 1;
    if (n < 2) return;

    double h[MAX_NODES], alpha[MAX_NODES], l[MAX_NODES], mu[MAX_NODES], z[MAX_NODES];

    for (int32_t i = 0; i <= n; i++) curve->spline_a[i] = curve->rates[i];
    for (int32_t i = 0; i <  n; i++) h[i] = curve->times[i+1] - curve->times[i];

    for (int32_t i = 1; i < n; i++)
        alpha[i] = (3.0 / h[i])   * (curve->spline_a[i+1] - curve->spline_a[i]) -
                   (3.0 / h[i-1]) * (curve->spline_a[i]   - curve->spline_a[i-1]);

    l[0] = 1.0; mu[0] = 0.0; z[0] = 0.0;
    l[n] = 1.0; z[n]  = 0.0;
    curve->spline_c[n] = 0.0;

    for (int32_t i = 1; i < n; i++) {
        l[i]  = 2.0 * (curve->times[i+1] - curve->times[i-1]) - h[i-1] * mu[i-1];
        mu[i] = h[i] / l[i];
        z[i]  = (alpha[i] - h[i-1] * z[i-1]) / l[i];
    }

    for (int32_t j = n - 1; j >= 0; j--) {
        curve->spline_c[j] = z[j] - mu[j] * curve->spline_c[j+1];
        curve->spline_b[j] = (curve->spline_a[j+1] - curve->spline_a[j]) / h[j] -
                              h[j] * (curve->spline_c[j+1] + 2.0 * curve->spline_c[j]) / 3.0;
        curve->spline_d[j] = (curve->spline_c[j+1] - curve->spline_c[j]) / (3.0 * h[j]);
    }
}

double interpolateCubicZero(const InterestRateCurve *curve, double t, int32_t idx)
{
    if (curve->numNodes < 3) return interpolateLogLinearDf(curve, t, idx);
    double dx = t - curve->times[idx];
    double r  = curve->spline_a[idx]
              + curve->spline_b[idx] * dx
              + curve->spline_c[idx] * dx * dx
              + curve->spline_d[idx] * dx * dx * dx;
    return exp(-r * t);
}


/* ====================================================================
 * Dispatch: routes t to the correct interpolation regime.
 *
 * The regime array on the curve is scanned in order; the first regime
 * whose upper_time_boundary >= t is used.  The last regime is always
 * selected as a catch-all for t beyond all boundaries.
 *
 * NOTE: the old hardwired bypass (direct call to getStepWiseDiscountFactor
 * for t <= last meeting) has been removed.  Stepwise OIS is now handled
 * uniformly through the regime dispatch by registering
 * interpolateStepWiseDF as regime[0] when a CB schedule is present.
 * ==================================================================== */
double getDiscountFactor(const InterestRateCurve *curve, double t)
{
    if (t <= 0.0) return 1.0;

    /* Binary search for the left-bracket node index */
    int32_t idx;
    if (t >= curve->times[curve->numNodes - 1]) {
        idx = curve->numNodes - 2;
    } else {
        int32_t low = 0, high = curve->numNodes - 1;
        while (high - low > 1) {
            int32_t mid = (low + high) / 2;
            if (curve->times[mid] > t) high = mid;
            else                        low  = mid;
        }
        idx = low;
    }

    for (int32_t i = 0; i < curve->numRegimes; i++) {
        if (t <= curve->regimes[i].upper_time_boundary || i == curve->numRegimes - 1)
            return curve->regimes[i].interp_func(curve, t, idx);
    }
    return interpolateLogLinearDf(curve, t, idx);
}


/* --- Implied forward rate between two dates ------------------------- */
double getForwardRate(const InterestRateCurve *curve, double t_start, double t_end)
{
    double dcf = t_end - t_start;
    if (dcf <= 0.0) return 0.0;
    double df_start = getDiscountFactor(curve, t_start);
    double df_end   = getDiscountFactor(curve, t_end);
    if (df_end <= 0.0) return 0.0;
    return (df_start / df_end - 1.0) / dcf;
}
