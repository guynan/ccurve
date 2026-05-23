#ifndef BLPAPI_FETCHER_H
#define BLPAPI_FETCHER_H

/*
 * blpapi_fetcher.h — pure C interface to the Bloomberg API.
 *
 * This header is C99-compatible.  The implementation (blpapi_fetcher.cpp)
 * is C++17 and links against the Bloomberg C++ SDK (blpapi3).
 * Build with: make bloomberg  (requires BLPAPI_HOME set)
 */

#include <time.h>
#include "dual_curve.h"   /* for MarketInstrument */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Session ──────────────────────────────────────────────────────────── */

typedef struct BlpSession BlpSession;

/* Create a session connected to host:port (typically "localhost":8194).
 * Returns NULL on failure. */
BlpSession *blp_session_create(const char *host, int port);

/* 1 if the session is connected and ready, 0 otherwise. */
int         blp_session_connected(const BlpSession *s);

/* Stop and free the session. Safe to call with NULL. */
void        blp_session_destroy(BlpSession *s);

/* ── Reference data (BDP) ─────────────────────────────────────────────── */

typedef struct {
    char   ticker[64];
    char   field[32];
    double value;
    int    ok;       /* 1 = success, 0 = error/missing */
    char   err[256];
} BlpRefResult;

/* Fetch one field value for each ticker.
 * tickers and fields are NULL-terminated arrays of C strings.
 * Returns a heap-allocated array of (num_tickers × num_fields) results;
 * caller frees with blp_free().  *out_count receives the array length.
 * Returns NULL on session error. */
BlpRefResult *blp_fetch_bdp(BlpSession   *s,
                             const char  **tickers,
                             const char  **fields,
                             int          *out_count,
                             int           timeout_ms);

/* ── Historical data (BDH) ───────────────────────────────────────────── */

typedef struct {
    time_t date;
    double value;
} BlpHistPoint;

typedef struct {
    char         ticker[64];
    char         field[32];
    BlpHistPoint *points;   /* heap-allocated; freed by blp_free_hist() */
    int           count;
    int           ok;
    char          err[256];
} BlpHistSeries;

/* Fetch a time series for each ticker.
 * start_date / end_date: "YYYYMMDD".
 * frequency: "DAILY", "WEEKLY", or "MONTHLY".
 * Returns a heap-allocated array of BlpHistSeries (*out_count entries).
 * Caller frees with blp_free_hist(). */
BlpHistSeries *blp_fetch_bdh(BlpSession   *s,
                              const char  **tickers,
                              const char   *field,
                              const char   *start_date,
                              const char   *end_date,
                              const char   *frequency,
                              int          *out_count,
                              int           timeout_ms);

/* ── Memory ───────────────────────────────────────────────────────────── */

void blp_free(void *ptr);
void blp_free_hist(BlpHistSeries *series, int count);

/* ── Convenience: fetch curve instruments directly ────────────────────── */

/*
 * Fetch SOFR/IBOR market instruments as of as_of_date ("YYYY-MM-DD").
 * Instruments written into out[0..n-1]; returns count written, -1 on error.
 * max_instruments should be at least 32 to capture a full curve (deposits,
 * 8 futures, 8 swaps, OIS nodes).
 *
 * Ticker universe fetched (USD example):
 *   Deposits : USDR3T Index, USDR1Z Index
 *   Futures  : SR1–SR8 Comdty (SOFR), ED1–ED8 Comdty (Eurodollar legacy)
 *   Swaps    : USSW1/2/3/5/7/10 Curncy
 *   OIS      : USOSFR1/2/3/5 Curncy
 *   Meetings : FOMCRX Index (target rate path / meeting dates)
 */
int blp_fetch_curve_instruments(BlpSession       *s,
                                MarketInstrument *out,
                                int               max_instruments,
                                const char       *as_of_date);

#ifdef __cplusplus
}
#endif

#endif /* BLPAPI_FETCHER_H */
