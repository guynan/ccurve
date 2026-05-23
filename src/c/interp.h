#ifndef INTERP_H
#define INTERP_H

#include <stdint.h>

/* Forward declaration — full struct defined in dual_curve.h.
 * Using the struct tag directly avoids a typedef redefinition when
 * dual_curve.h includes this header and then defines the full type. */
struct InterestRateCurve;

/* All interpolation functions share this signature.
 * curve is const: evaluation never modifies curve state. */
typedef double (*InterpolationFunction)(const struct InterestRateCurve *curve,
                                        double t, int32_t idx);

typedef struct {
    double                upper_time_boundary;
    InterpolationFunction interp_func;
} InterpolationRegime;

/* --- Log-DF cubic spline (Hagan & West log-space, natural BCs) --- */
void   setupLogDfCubicSpline(struct InterestRateCurve *curve);
double interpolateLogDfCubic(const struct InterestRateCurve *curve,
                             double t, int32_t idx);

/* --- Monotone-convex interpolation (Hagan & West 2006) --- */
void   setupMonotoneConvex(struct InterestRateCurve *curve);
double interpolateMonotoneConvex(const struct InterestRateCurve *curve,
                                 double t, int32_t idx);

/* --- Log-linear on discount factors --- */
double interpolateLogLinearDf(const struct InterestRateCurve *curve,
                              double t, int32_t idx);

/* --- Stepwise OIS (central bank meeting schedule) ---
 * interpolateStepWiseOIS: returns the active rate at time t (diagnostic use).
 * getStepWiseDiscountFactor: integrates the step function to produce a DF.
 * interpolateStepWiseDF: regime-compatible wrapper around getStepWiseDiscountFactor. */
double interpolateStepWiseOIS(const struct InterestRateCurve *curve,
                              double t, int32_t idx);
double getStepWiseDiscountFactor(const struct InterestRateCurve *curve, double t);
double interpolateStepWiseDF(const struct InterestRateCurve *curve,
                             double t, int32_t idx);

/* --- Parabolic zero rate spline --- */
void   setupParabolicSpline(struct InterestRateCurve *curve);
double interpolateParabolicZero(const struct InterestRateCurve *curve,
                                double t, int32_t idx);

/* --- Natural cubic spline on zero rates --- */
void   setupCubicSpline(struct InterestRateCurve *curve);
double interpolateCubicZero(const struct InterestRateCurve *curve,
                            double t, int32_t idx);

/* --- Dispatch: routes to the correct regime for time t --- */
double getDiscountFactor(const struct InterestRateCurve *curve, double t);

/* --- Forward rate between two times using the curve's discount factors --- */
double getForwardRate(const struct InterestRateCurve *curve,
                     double t_start, double t_end);

#endif /* INTERP_H */
