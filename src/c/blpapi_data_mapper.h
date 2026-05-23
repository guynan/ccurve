#ifndef BLPAPI_DATA_MAPPER_H
#define BLPAPI_DATA_MAPPER_H

/*
 * blpapi_data_mapper.h — ticker→MarketInstrument mapping declarations.
 *
 * Maps Bloomberg tickers and raw field values into MarketInstrument structs
 * ready for bootstrapCurve().  All functions are C-callable (extern "C").
 */

#include <time.h>
#include "dual_curve.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── IMM date resolution ──────────────────────────────────────────────── */

/*
 * Parse a futures ticker suffix and return the IMM contract start date as
 * a year fraction from ref_date.
 *
 * Examples:
 *   "SR1 Comdty" → next quarterly IMM date (3rd Wed of Mar/Jun/Sep/Dec)
 *   "ED3 Comdty" → 3rd IMM contract from ref_date
 *   "EDM6 Comdty" → June 2026 IMM date
 *
 * Returns the year fraction, or -1.0 on parse failure.
 */
double blp_imm_start(const char *ticker, time_t ref_date);

/* IMM contract end = start + 0.25 (one quarter). */
double blp_imm_end(const char *ticker, time_t ref_date);

/* ── Ticker classification ────────────────────────────────────────────── */

/*
 * Identify instrument type from Bloomberg ticker.
 * Returns one of: DEPOSIT, FUTURE, SWAP, OIS_SWAP, or -1 if unrecognised.
 */
int blp_ticker_type(const char *ticker);

/* ── Instrument builder ───────────────────────────────────────────────── */

/*
 * Populate *out from a single Bloomberg ticker + its PX_LAST / MID / YLD_MID
 * field value.  ref_date is used to derive year fractions.
 *
 * Returns 0 on success, -1 if the ticker is unrecognised or the value is
 * out of a plausible range.
 */
int blp_build_instrument(MarketInstrument *out,
                         const char       *ticker,
                         double            field_value,
                         time_t            ref_date);

/* ── Bulk mapper ──────────────────────────────────────────────────────── */

/*
 * Map an array of (ticker, value) pairs into MarketInstrument[].
 * Instruments are written in maturity order.
 * Returns number written; -1 on fatal error.
 */
typedef struct {
    char   ticker[64];
    double value;
} BlpRawQuote;

int blp_map_instruments(const BlpRawQuote *quotes,
                        int                num_quotes,
                        MarketInstrument  *out,
                        int                max_out,
                        time_t             ref_date);

#ifdef __cplusplus
}
#endif

#endif /* BLPAPI_DATA_MAPPER_H */
