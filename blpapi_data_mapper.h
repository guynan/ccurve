/*
 * blpapi_data_mapper.h  —  Ticker classification and date/tenor utilities.
 *
 * Pure C header; no C++ constructs.  Include this from C, C++, or any
 * language with a C FFI.  The implementation is in blpapi_data_mapper.cpp
 * and exports all symbols via extern "C".
 *
 * This module has no Bloomberg SDK dependency — it only knows about ticker
 * naming conventions used in blpapi_fetcher.cpp and related tools.
 */

#ifndef BLPAPI_DATA_MAPPER_H
#define BLPAPI_DATA_MAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  Ticker classification                                                     */
/* ======================================================================== */

/**
 * Broad category of a Bloomberg ticker, inferred from its prefix.
 */
typedef enum {
    TICKER_UNKNOWN  = 0,  /**< Unrecognised or unsupported ticker.            */
    TICKER_DEPOSIT,       /**< Money-market deposit  (e.g. "USDR3T Index").   */
    TICKER_FUTURE,        /**< Interest rate future  (e.g. "SR1 Comdty").     */
    TICKER_SWAP,          /**< Vanilla IRS            (e.g. "USSW5 Curncy"). */
    TICKER_OIS_SWAP       /**< OIS / SOFR swap        (e.g. "USOSFR3 Curncy"). */
} TickerClass;

/**
 * Classify a Bloomberg ticker string by prefix matching.
 *
 * Rules applied in order (first match wins):
 *   "USOSFR"         → TICKER_OIS_SWAP
 *   "USSW"           → TICKER_SWAP         (must not start with "USOSFR")
 *   "SR" + digit     → TICKER_FUTURE
 *   "ED" + digit     → TICKER_FUTURE
 *   "USDR"           → TICKER_DEPOSIT
 *   anything else    → TICKER_UNKNOWN
 *
 * The ticker may optionally include a Bloomberg yellow-key suffix
 * (e.g. " Curncy", " Index", " Comdty") — the suffix is ignored.
 *
 * @param ticker  Bloomberg security identifier string (NUL-terminated).
 * @return        TickerClass enum value.
 */
TickerClass classify_ticker(const char *ticker);


/* ======================================================================== */
/*  IMM date calculation                                                      */
/* ======================================================================== */

/**
 * Compute the Julian Day Number of the 3rd Wednesday of a given month/year.
 *
 * This is the standard IMM (International Monetary Market) expiry date used
 * for SOFR and Eurodollar futures contracts.
 *
 * Month codes follow CME / Bloomberg futures convention:
 *   F=Jan  G=Feb  H=Mar  J=Apr  K=May  M=Jun
 *   N=Jul  Q=Aug  U=Sep  V=Oct  X=Nov  Z=Dec
 *
 * Algorithm:
 *   1. Compute JDN of the 1st of the month.
 *   2. day_of_week = JDN % 7  (0=Mon, 1=Tue, 2=Wed, …, 6=Sun).
 *   3. days_to_first_wednesday = (2 - dow + 7) % 7.
 *   4. 3rd Wednesday = JDN_of_1st + days_to_first_wednesday + 14.
 *
 * @param month_code  Single uppercase character from the set above.
 * @param year_4digit Four-digit Gregorian year (e.g. 2026).
 * @return            Julian Day Number of the 3rd Wednesday, or -1 if
 *                    month_code is not one of the recognised codes.
 */
long imm_date_jdn(char month_code, int year_4digit);


/* ======================================================================== */
/*  Tenor extraction                                                          */
/* ======================================================================== */

/**
 * Parse the tenor (in years as a double) from a swap ticker.
 *
 * Supported prefixes:
 *   "USSW"   — vanilla USD IRS, e.g. "USSW5"  → 5.0, "USSW10" → 10.0
 *   "USOSFR" — OIS / SOFR swap,  e.g. "USOSFR3" → 3.0
 *
 * The suffix (e.g. " Curncy") is ignored.
 *
 * @param ticker  Bloomberg security identifier string.
 * @return        Tenor in years (positive double), or 0.0 if the ticker
 *                does not match a known swap prefix or the numeric part
 *                cannot be parsed.
 */
double swap_tenor_years(const char *ticker);

/**
 * Parse the tenor (in years as a double) from a deposit ticker.
 *
 * Supported suffixes after the "USDR" prefix:
 *   "3T"   → 3 months    → 0.25
 *   "1W"   → 1 week      → 1.0/52
 *   "2W"   → 2 weeks     → 2.0/52
 *   "1M"   → 1 month     → 1.0/12
 *   "2M"   → 2 months    → 2.0/12
 *   "3M"   → 3 months    → 3.0/12   (= 0.25)
 *   "6M"   → 6 months    → 6.0/12
 *   "1Y"   → 1 year      → 1.0
 *
 * The "T" suffix after a digit means "months" in Bloomberg deposit convention
 * (e.g. "3T" = 3-month).  "W", "M", "Y" are week, month, year respectively.
 * The ticker yellow-key suffix (e.g. " Index") is ignored.
 *
 * @param ticker  Bloomberg security identifier string (e.g. "USDR3T Index").
 * @return        Tenor in years, or 0.0 if unparseable.
 */
double deposit_tenor_years(const char *ticker);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BLPAPI_DATA_MAPPER_H */
