/*
 * blpapi_data_mapper.cpp  —  Ticker classification and tenor/date utilities.
 *
 * No Bloomberg SDK dependency; pure string/arithmetic logic.
 * All public symbols are exported as plain C.
 *
 * Build note: compiled as C++17 and linked into the shared library together
 * with blpapi_fetcher.cpp.  All public entry points are wrapped in
 * extern "C" so they have C linkage and are callable from any language.
 */

#include "blpapi_data_mapper.h"

#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>

/* ======================================================================== */
/*  Internal helpers (C++ linkage, not exported)                             */
/* ======================================================================== */

namespace {

/**
 * Case-insensitively test whether 'str' starts with 'prefix'.
 * The comparison is byte-by-byte; the ticker is expected to be ASCII.
 */
inline bool starts_with_ci(const char *str, const char *prefix) noexcept
{
    if (!str || !prefix) return false;
    while (*prefix) {
        if (std::toupper(static_cast<unsigned char>(*str)) !=
            std::toupper(static_cast<unsigned char>(*prefix)))
            return false;
        ++str;
        ++prefix;
    }
    return true;
}

/**
 * Test whether character at position pos in str is an ASCII digit.
 * Returns false if str is shorter than pos+1 characters.
 */
inline bool char_is_digit(const char *str, std::size_t pos) noexcept
{
    if (!str) return false;
    const std::size_t len = std::strlen(str);
    if (pos >= len) return false;
    return std::isdigit(static_cast<unsigned char>(str[pos])) != 0;
}

/**
 * Map IMM month code letter to 1-based calendar month number.
 * Returns 0 for an unrecognised code.
 */
int imm_code_to_month(char code) noexcept
{
    switch (std::toupper(static_cast<unsigned char>(code))) {
    case 'F': return  1;  /* January   */
    case 'G': return  2;  /* February  */
    case 'H': return  3;  /* March     */
    case 'J': return  4;  /* April     */
    case 'K': return  5;  /* May       */
    case 'M': return  6;  /* June      */
    case 'N': return  7;  /* July      */
    case 'Q': return  8;  /* August    */
    case 'U': return  9;  /* September */
    case 'V': return 10;  /* October   */
    case 'X': return 11;  /* November  */
    case 'Z': return 12;  /* December  */
    default:  return  0;
    }
}

/**
 * Compute the Julian Day Number for the 1st day of a given month and year,
 * using the proleptic Gregorian calendar formula used throughout this
 * project (see date_utils.c).
 */
long jdn_of_month_start(int month, int year) noexcept
{
    long y = year, m = month;
    if (m <= 2) { y--; m += 12; }
    long a = y / 100;
    long b = a / 4;
    long c = 2 - a + b;
    long e = static_cast<long>(365.25  * static_cast<double>(y + 4716));
    long f = static_cast<long>(30.6001 * static_cast<double>(m + 1));
    /* day = 1 */
    return c + 1L + e + f - 1524L;
}

} /* anonymous namespace */


/* ======================================================================== */
/*  Public C interface                                                        */
/* ======================================================================== */

extern "C" {

/* ------------------------------------------------------------------------ */
/*  classify_ticker                                                           */
/* ------------------------------------------------------------------------ */

TickerClass classify_ticker(const char *ticker)
{
    if (!ticker || ticker[0] == '\0')
        return TICKER_UNKNOWN;

    /*
     * Order matters: "USOSFR" must be tested before "USSW" because
     * "USOSFR" also starts with "US".  Likewise "USSW" must not match
     * "USOSFR*" tickers.
     */

    /* OIS / SOFR swap: "USOSFR..." */
    if (starts_with_ci(ticker, "USOSFR"))
        return TICKER_OIS_SWAP;

    /* Vanilla IRS: "USSW" followed by a digit */
    if (starts_with_ci(ticker, "USSW") && char_is_digit(ticker, 4))
        return TICKER_SWAP;

    /* SOFR futures: "SR" followed by a digit (SR1–SR8) */
    if (starts_with_ci(ticker, "SR") && char_is_digit(ticker, 2))
        return TICKER_FUTURE;

    /* Eurodollar futures: "ED" followed by a digit */
    if (starts_with_ci(ticker, "ED") && char_is_digit(ticker, 2))
        return TICKER_FUTURE;

    /* USD deposit: "USDR..." */
    if (starts_with_ci(ticker, "USDR"))
        return TICKER_DEPOSIT;

    return TICKER_UNKNOWN;
}


/* ------------------------------------------------------------------------ */
/*  imm_date_jdn                                                              */
/* ------------------------------------------------------------------------ */

long imm_date_jdn(char month_code, int year_4digit)
{
    const int month = imm_code_to_month(month_code);
    if (month == 0)
        return -1L;   /* unrecognised month code */

    /*
     * Algorithm (documented in blpapi_data_mapper.h):
     *
     * 1. JDN of the 1st of month/year.
     * 2. day_of_week = JDN % 7   (0=Mon, 1=Tue, 2=Wed, 3=Thu, 4=Fri,
     *                              5=Sat, 6=Sun) — matches the convention
     *    in isWeekend() in date_utils.c.
     * 3. days_to_first_wednesday = (2 - dow + 7) % 7
     *    (Wednesday is dow==2; we want the smallest non-negative offset).
     * 4. 3rd Wednesday JDN = JDN_of_1st + days_to_first_wednesday + 14.
     */

    const long jdn1 = jdn_of_month_start(month, year_4digit);
    const long dow  = jdn1 % 7L;                     /* 0=Mon … 6=Sun   */
    const long to_first_wed = (2L - dow + 7L) % 7L;  /* offset to first Wed */
    return jdn1 + to_first_wed + 14L;                 /* +14 for 3rd Wed */
}


/* ------------------------------------------------------------------------ */
/*  swap_tenor_years                                                          */
/* ------------------------------------------------------------------------ */

double swap_tenor_years(const char *ticker)
{
    if (!ticker)
        return 0.0;

    const char *num_start = nullptr;

    /*
     * Determine where the numeric tenor begins:
     *   "USOSFR" → 6 chars prefix, e.g. "USOSFR3" → 3
     *   "USSW"   → 4 chars prefix, e.g. "USSW10"  → 10
     */
    if (starts_with_ci(ticker, "USOSFR"))
        num_start = ticker + 6;
    else if (starts_with_ci(ticker, "USSW"))
        num_start = ticker + 4;
    else
        return 0.0;

    /* num_start must point at a digit */
    if (!num_start || !std::isdigit(static_cast<unsigned char>(*num_start)))
        return 0.0;

    /*
     * Parse the integer tenor.  strtol stops at the first non-digit
     * (e.g. the space before " Curncy"), which is correct.
     */
    char *end = nullptr;
    const long years = std::strtol(num_start, &end, 10);
    if (end == num_start || years <= 0)
        return 0.0;

    return static_cast<double>(years);
}


/* ------------------------------------------------------------------------ */
/*  deposit_tenor_years                                                       */
/* ------------------------------------------------------------------------ */

double deposit_tenor_years(const char *ticker)
{
    if (!ticker)
        return 0.0;

    /* Must begin with "USDR" */
    if (!starts_with_ci(ticker, "USDR"))
        return 0.0;

    const char *suffix = ticker + 4;   /* e.g. "3T", "1W", "1M", "1Y", … */
    if (!suffix || suffix[0] == '\0')
        return 0.0;

    /*
     * Parse the leading integer count (1 or 2 digits).
     */
    char *end = nullptr;
    const long count = std::strtol(suffix, &end, 10);
    if (!end || end == suffix || count <= 0)
        return 0.0;

    /*
     * 'end' now points at the unit character (or a space/NUL if missing).
     * Bloomberg deposit tickers use:
     *   'T' after a digit → months  ("3T" = 3-month)
     *   'W'               → weeks
     *   'M'               → months
     *   'Y'               → years
     */
    const char unit = static_cast<char>(
        std::toupper(static_cast<unsigned char>(*end)));

    switch (unit) {
    case 'T':   /* month, Bloomberg convention (e.g. "3T" = 3-month deposit) */
    case 'M':
        return static_cast<double>(count) / 12.0;

    case 'W':
        return static_cast<double>(count) / 52.0;

    case 'Y':
        return static_cast<double>(count);

    default:
        return 0.0;
    }
}

} /* extern "C" */
