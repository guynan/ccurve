/*
 * blpapi_fetcher.h  —  Pure C public interface to the Bloomberg data fetcher.
 *
 * This header is intentionally free of C++.  It may be included from C, C++,
 * or any language with a C FFI.  The implementation (blpapi_fetcher.cpp) is a
 * C++ translation unit that links against the Bloomberg C++ SDK, but all
 * symbols crossing the shared-library boundary are plain C.
 *
 * Build the shared library:
 *   g++ -std=c++17 -fPIC -shared \
 *       -I${BLPAPI_HOME}/include \
 *       blpapi_fetcher.cpp blpapi_data_mapper.cpp \
 *       date_utils.c \
 *       -L${BLPAPI_HOME}/lib -lblpapi3_64 \
 *       -o libblp_fetcher.so
 *
 * Link your consumer:
 *   gcc my_app.c -L. -lblp_fetcher -Wl,-rpath,'$ORIGIN' -o my_app
 */

#ifndef BLPAPI_FETCHER_H
#define BLPAPI_FETCHER_H

#include <time.h>
#include <stdint.h>

#include "dual_curve.h"   /* MarketInstrument, InstrumentType, DayCountFraction, etc. */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  Session lifecycle                                                         */
/* ======================================================================== */

/**
 * Opaque handle to a Bloomberg API session.
 * Allocated by blp_session_create(); released by blp_session_destroy().
 * All blp_fetch_* functions require a successfully connected session.
 */
typedef struct BlpSession BlpSession;

/**
 * Create and connect a Bloomberg session.
 *
 * @param host  Bloomberg server host name or IP (e.g. "localhost").
 * @param port  Bloomberg server port (typically 8194).
 * @return      Newly allocated BlpSession, or NULL if allocation failed.
 *              Call blp_session_connected() to confirm the connection
 *              succeeded before issuing any data requests.
 */
BlpSession *blp_session_create(const char *host, int port);

/**
 * Query whether the session is connected and the refdata service is open.
 *
 * @return 1 if connected and ready, 0 otherwise.
 */
int blp_session_connected(const BlpSession *s);

/**
 * Stop, destroy, and free a Bloomberg session.
 * Passing NULL is safe (no-op).
 */
void blp_session_destroy(BlpSession *s);


/* ======================================================================== */
/*  BDP — Bloomberg Data Point (reference / snapshot)                        */
/* ======================================================================== */

/**
 * Result record for one (ticker, field) pair from a BDP request.
 */
typedef struct {
    char   ticker[64];      /**< Security identifier as supplied by the caller. */
    char   field[32];       /**< Field mnemonic as supplied by the caller.       */
    double value;           /**< Numeric value; 0.0 when the field is non-numeric
                             *   or the request failed.                          */
    char   str_value[64];   /**< String representation (dates, descriptions, …).
                             *   Always NUL-terminated; may be empty.            */
    int    ok;              /**< 1 = data populated; 0 = security not found or
                             *   field unavailable (see err for detail).         */
    char   err[256];        /**< Human-readable error message when ok==0.        */
} BlpRefResult;

/**
 * Fetch reference data (BDP) for a set of tickers and fields.
 *
 * Sends a single ReferenceDataRequest containing all tickers and fields.
 * Polls the event queue until a RESPONSE event arrives or timeout_ms elapses.
 *
 * @param s           Connected BlpSession.
 * @param tickers     NULL-terminated array of security identifiers
 *                    (e.g. {"USSW5 Curncy", "SR1 Comdty", NULL}).
 * @param fields      NULL-terminated array of field mnemonics
 *                    (e.g. {"MID", "PX_LAST", NULL}).
 * @param out_count   Receives the total number of (ticker, field) results
 *                    written into the returned array.
 * @param timeout_ms  Maximum milliseconds to wait for a RESPONSE event.
 *                    Use 0 for the BLPAPI library default (no timeout).
 * @return            Heap-allocated array of BlpRefResult with
 *                    (*out_count) elements.  The caller MUST free this
 *                    array with blp_free().
 *                    Returns NULL with *out_count==0 on session error.
 */
BlpRefResult *blp_fetch_bdp(BlpSession   *s,
                             const char  **tickers,
                             const char  **fields,
                             int          *out_count,
                             int           timeout_ms);


/* ======================================================================== */
/*  BDH — Bloomberg Data History (time series)                               */
/* ======================================================================== */

/** One date/value observation in a historical time series. */
typedef struct {
    time_t date;    /**< UTC midnight of the observation date. */
    double value;   /**< Field value on that date.             */
} BlpHistPoint;

/**
 * Historical time series for one (ticker, field) combination.
 */
typedef struct {
    char         ticker[64];  /**< Security identifier.                       */
    char         field[32];   /**< Field mnemonic.                            */
    BlpHistPoint *points;     /**< Sorted ascending by date; heap-allocated.  */
    int           count;      /**< Number of elements in points[].            */
    int           ok;         /**< 1 = data populated; 0 = error.             */
    char          err[256];   /**< Error description when ok==0.              */
} BlpHistSeries;

/**
 * Fetch historical time series (BDH) for one or more tickers.
 *
 * Sends a single HistoricalDataRequest.  The BLPAPI library returns one
 * HistoricalDataResponse message per security; all are consumed before
 * returning.
 *
 * @param s           Connected BlpSession.
 * @param tickers     NULL-terminated array of security identifiers.
 * @param field       Single field mnemonic (e.g. "PX_LAST").
 * @param start_date  Start date in "YYYYMMDD" format (Bloomberg native).
 * @param end_date    End date in "YYYYMMDD" format.
 * @param frequency   Periodicity override: "DAILY", "WEEKLY", or "MONTHLY".
 *                    Pass NULL or "" to use Bloomberg's default.
 * @param out_count   Receives the number of BlpHistSeries records returned.
 * @param timeout_ms  Maximum milliseconds to wait per event; 0 = no timeout.
 * @return            Heap-allocated array of BlpHistSeries with (*out_count)
 *                    elements.  Each series has its own heap-allocated
 *                    points[] array.  Call blp_free_hist() to release
 *                    the entire structure.  Returns NULL on session error.
 */
BlpHistSeries *blp_fetch_bdh(BlpSession   *s,
                              const char  **tickers,
                              const char   *field,
                              const char   *start_date,
                              const char   *end_date,
                              const char   *frequency,
                              int          *out_count,
                              int           timeout_ms);


/* ======================================================================== */
/*  Memory management                                                         */
/* ======================================================================== */

/**
 * Free any pointer returned by blp_fetch_bdp() or any other blp_*
 * function that allocates a flat array.
 * Passing NULL is safe (no-op).
 */
void blp_free(void *ptr);

/**
 * Free a BlpHistSeries array (including each series' points[] sub-array).
 *
 * @param series  Array returned by blp_fetch_bdh().
 * @param count   Value written into *out_count by blp_fetch_bdh().
 */
void blp_free_hist(BlpHistSeries *series, int count);


/* ======================================================================== */
/*  Convenience: USD SOFR curve instrument fetch                              */
/* ======================================================================== */

/**
 * Fetch a complete set of USD SOFR curve market instruments from Bloomberg
 * in a single BDP request and populate a MarketInstrument array ready for
 * use with bootstrapCurve() / bootstrapOisCurve().
 *
 * Instrument universe fetched:
 *   Deposits  : USDR3T Index            (YLD_YTM_MID)  — 3-month SOFR deposit
 *   Futures   : SR1–SR8 Comdty          (PX_LAST, LAST_TRADEABLE_DT)
 *   Swaps     : USSW1/2/3/5/7/10 Curncy (MID)
 *   OIS swaps : USOSFR1/2/3/5 Curncy   (MID)
 *
 * All year fractions are computed relative to as_of_date using Act/365.
 * Market conventions applied:
 *   Deposits : fixedDcf=DCF_ACT_360, bda=BDA_MODIFIED_FOLLOWING, cal="USD"
 *   Futures  : startTime = maturity - 0.25, paymentFrequency=4
 *   Swaps    : fixedDcf=DCF_30_360, floatDcf=DCF_ACT_360, payFreq=2
 *   OIS      : fixedDcf=DCF_ACT_360, floatDcf=DCF_ACT_360, payFreq=4
 *
 * @param s               Connected BlpSession.
 * @param out             Caller-supplied MarketInstrument array.
 * @param max_instruments Capacity of out[]; must be >= 15 for the full set.
 * @param as_of_date      Curve date in "YYYY-MM-DD" format.
 * @return                Number of instruments written into out[],
 *                        or -1 if the session is not connected.
 */
int blp_fetch_curve_instruments(BlpSession       *s,
                                MarketInstrument *out,
                                int               max_instruments,
                                const char       *as_of_date);

/* ======================================================================== */
/*  Convenience: NZD dual-curve instrument fetch                             */
/* ======================================================================== */

/**
 * Fetch the full NZD dual-curve instrument set from Bloomberg in a single
 * BDP request.  Returns instruments tagged by type so the caller can split
 * them into the two independent bootstrap inputs:
 *
 *   OIS_SWAP  → NZONIA OIS discount curve  (bootstrapOisCurve)
 *   DEPOSIT / FUTURE / SWAP → BKBM forward curve  (bootstrapCurve vs OIS)
 *
 * Instrument universe (19 instruments, OIS first then BKBM in order):
 *   OIS   : NDSO{1..6} Curncy        (MID, MATURITY) — 1Y–6Y NZONIA OIS swaps
 *   Depo  : NDBB3M Curncy            (MID)           — 3M BKBM bank bill
 *   Fut   : ZB1–ZB4 Comdty           (PX_LAST, LAST_TRADEABLE_DT)
 *   Swap  : NDSWAP{3,4,5,6,7,10,12,15} Curncy (MID)  — quarterly BKBM IRS
 *
 * NZD market conventions applied to all instruments:
 *   fixedDcf=DCF_ACT_365, floatDcf=DCF_ACT_365,
 *   bda=BDA_MODIFIED_FOLLOWING, cal="NZD"
 *   BKBM IRS / futures paymentFrequency=4 (quarterly)
 *
 * @param s               Connected BlpSession.
 * @param out             Caller-supplied MarketInstrument array.
 * @param max_instruments Capacity of out[]; must be >= 19 for the full set.
 * @param as_of_date      Curve date in "YYYY-MM-DD" format.
 * @return                Number of instruments written into out[],
 *                        or -1 if the session is not connected.
 */
int blp_fetch_nzd_curve_instruments(BlpSession       *s,
                                     MarketInstrument *out,
                                     int               max_instruments,
                                     const char       *as_of_date);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BLPAPI_FETCHER_H */
