/*
 * blpapi_data_mapper.cpp — ticker→MarketInstrument mapping.
 *
 * Implements blpapi_data_mapper.h.  Handles:
 *   - Ticker classification (deposit / future / swap / OIS)
 *   - IMM date resolution from sequential (SR1…SR8) and named (EDM6) tickers
 *   - Instrument population including year fractions, dcf, conventions
 *   - Bulk mapping with maturity-order sorting
 */

#include "blpapi_data_mapper.h"
#include "dual_curve.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cctype>
#include <algorithm>
#include <vector>
#include <string>

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Year fraction between two time_t values (Act/365). */
static double year_frac(time_t from, time_t to)
{
    double diff = difftime(to, from);
    return diff / (365.25 * 86400.0);
}

/* Return time_t for YYYY-MM-DD at noon UTC. */
static time_t make_date(int y, int m, int d)
{
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    return mktime(&t);
}

/* Return time_t for the Nth Wednesday of month m of year y. */
static time_t nth_wednesday(int y, int m, int n)
{
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = 1;
    mktime(&t);

    /* Advance to first Wednesday (wday==3) */
    int days_to_wed = (3 - t.tm_wday + 7) % 7;
    t.tm_mday += days_to_wed + (n - 1) * 7;
    mktime(&t);
    return make_date(y, t.tm_mon + 1, t.tm_mday);
}

/* IMM months for SOFR/ED quarterly contracts. */
static const int IMM_MONTHS[] = { 3, 6, 9, 12 };

/*
 * Return the Nth upcoming IMM date on or after ref_date.
 * n=1 → next IMM, n=2 → second, etc.
 */
static time_t nth_imm_date(time_t ref_date, int n)
{
    struct tm t = *localtime(&ref_date);
    int y = t.tm_year + 1900;
    int m = t.tm_mon  + 1;

    /* Walk quarterly IMM months until we have n dates >= ref_date */
    int found = 0;
    for (int yy = y; yy <= y + 4; ++yy) {
        for (int qi = 0; qi < 4; ++qi) {
            int mm = IMM_MONTHS[qi];
            if (yy == y && mm < m) continue;
            time_t imm = nth_wednesday(yy, mm, 3);
            if (imm >= ref_date) {
                ++found;
                if (found == n) return imm;
            }
        }
    }
    return static_cast<time_t>(-1);
}

/*
 * Parse named IMM ticker suffix e.g. "M6" → month M=June, year 6→2026.
 * Returns time_t of the 3rd Wednesday, or -1 on failure.
 */
static time_t named_imm_date(char month_char, int year_digit, time_t ref_date)
{
    static const char MONTH_CODES[] = "FGHJKMNQUVXZ";  /* Jan..Dec */
    const char *p = strchr(MONTH_CODES, (char)toupper(month_char));
    if (!p) return static_cast<time_t>(-1);
    int m = static_cast<int>(p - MONTH_CODES) + 1;

    struct tm rt = *localtime(&ref_date);
    int ref_year = rt.tm_year + 1900;
    /* Resolve 1-digit year: pick nearest decade */
    int y = (ref_year / 10) * 10 + year_digit;
    if (y < ref_year) y += 10;

    return nth_wednesday(y, m, 3);
}

/* ── Ticker classification ────────────────────────────────────────────── */

extern "C"
int blp_ticker_type(const char *ticker)
{
    if (!ticker) return -1;
    std::string t = ticker;

    /* Deposits: "USDR3T Index", "USDR1Z Index" */
    if (t.find("USDR") == 0 && t.find("Index") != std::string::npos)
        return static_cast<int>(DEPOSIT);

    /* SOFR futures: "SR1 Comdty" … "SR8 Comdty" */
    if ((t.find("SR") == 0 || t.find("ED") == 0) &&
         t.find("Comdty") != std::string::npos)
        return static_cast<int>(FUTURE);

    /* OIS swaps: "USOSFR* Curncy" */
    if (t.find("USOSFR") == 0 && t.find("Curncy") != std::string::npos)
        return static_cast<int>(OIS_SWAP);

    /* Par swaps: "USSW* Curncy" */
    if (t.find("USSW") == 0 && t.find("Curncy") != std::string::npos)
        return static_cast<int>(SWAP);

    return -1;
}

/* ── IMM year fraction helpers ────────────────────────────────────────── */

extern "C"
double blp_imm_start(const char *ticker, time_t ref_date)
{
    if (!ticker) return -1.0;
    std::string t = ticker;

    /* Sequential: "SR1 Comdty" or "ED3 Comdty" */
    bool is_sr = (t.find("SR") == 0);
    bool is_ed = (t.find("ED") == 0);
    if ((is_sr || is_ed) && t.find("Comdty") != std::string::npos) {
        /* Try sequential index first: "SR1", "ED3" */
        int seq = 0;
        if (sscanf(ticker + 2, "%d", &seq) == 1 && seq >= 1 && seq <= 12) {
            time_t imm = nth_imm_date(ref_date, seq);
            if (imm == static_cast<time_t>(-1)) return -1.0;
            return year_frac(ref_date, imm);
        }
        /* Named: "EDM6 Comdty" */
        char mc = ticker[2];
        int  yd = 0;
        if (isalpha(mc) && sscanf(ticker + 3, "%d", &yd) == 1) {
            time_t imm = named_imm_date(mc, yd, ref_date);
            if (imm == static_cast<time_t>(-1)) return -1.0;
            return year_frac(ref_date, imm);
        }
    }
    return -1.0;
}

extern "C"
double blp_imm_end(const char *ticker, time_t ref_date)
{
    double s = blp_imm_start(ticker, ref_date);
    return (s < 0.0) ? -1.0 : s + 0.25;
}

/* ── Swap maturity from ticker ────────────────────────────────────────── */

/*
 * Extract year tenor from tickers like "USSW1", "USOSFR10".
 * Returns tenor in years, or 0 on failure.
 */
static double swap_tenor_years(const char *ticker)
{
    /* Skip alpha prefix, parse trailing integer */
    int i = 0;
    while (ticker[i] && isalpha((unsigned char)ticker[i])) ++i;
    int yr = 0;
    if (sscanf(ticker + i, "%d", &yr) == 1 && yr > 0)
        return static_cast<double>(yr);
    return 0.0;
}

/* ── Instrument builder ───────────────────────────────────────────────── */

extern "C"
int blp_build_instrument(MarketInstrument *out,
                          const char       *ticker,
                          double            field_value,
                          time_t            ref_date)
{
    if (!out || !ticker) return -1;
    memset(out, 0, sizeof(*out));

    int itype = blp_ticker_type(ticker);
    if (itype < 0) return -1;

    out->type = static_cast<InstrumentType>(itype);

    switch (itype) {
    case DEPOSIT: {
        /* field_value: yield in percent (e.g. 5.32 for 5.32%) → decimal */
        double rate = field_value / 100.0;
        if (rate < 0.0 || rate > 0.25) return -1;  /* sanity */
        out->startTime = 0.0;
        /* USDR3T = 3-month */
        if (strstr(ticker, "3T")) {
            out->maturity = 0.25;
        } else if (strstr(ticker, "1Z")) {
            out->maturity = 1.0 / 365.0;           /* overnight */
        } else {
            out->maturity = 0.25;                   /* default 3M */
        }
        out->rate             = rate;
        out->paymentFrequency = 4;
        out->fixedDcf         = static_cast<DayCountFraction>(0); /* ACT/365 */
        snprintf(out->calendarName, sizeof(out->calendarName), "USD");
        break;
    }
    case FUTURE: {
        /* field_value: price, e.g. 94.68 → implied rate = (100 - 94.68)/100 */
        if (field_value < 85.0 || field_value > 100.0) return -1;
        double t_start = blp_imm_start(ticker, ref_date);
        double t_end   = t_start + 0.25;
        if (t_start < 0.0) return -1;
        out->startTime        = t_start;
        out->maturity         = t_end;
        out->price            = field_value;
        out->paymentFrequency = 4;
        snprintf(out->calendarName, sizeof(out->calendarName), "USD");
        break;
    }
    case SWAP:
    case OIS_SWAP: {
        /* field_value: mid rate in percent */
        double rate = field_value / 100.0;
        if (rate < 0.0 || rate > 0.30) return -1;
        double tenor = swap_tenor_years(ticker);
        if (tenor <= 0.0) return -1;
        out->startTime        = 0.0;
        out->maturity         = tenor;
        out->rate             = rate;
        out->paymentFrequency = (itype == static_cast<int>(OIS_SWAP)) ? 1 : 2;
        out->fixedDcf         = static_cast<DayCountFraction>(0);
        out->floatDcf         = static_cast<DayCountFraction>(0);
        snprintf(out->calendarName, sizeof(out->calendarName), "USD");
        /* OIS float leg: overnight compound, 2bd payment lag */
        if (itype == static_cast<int>(OIS_SWAP)) {
            out->floatIndex.indexType      = RATE_IDX_OIS_COMPOUND;
            out->floatIndex.paymentLagDays = 2;
            out->floatIndex.tenorYears     = 1.0 / 365.0;
        }
        break;
    }
    default:
        return -1;
    }

    return 0;
}

/* ── Bulk mapper ──────────────────────────────────────────────────────── */

extern "C"
int blp_map_instruments(const BlpRawQuote *quotes,
                         int                num_quotes,
                         MarketInstrument  *out,
                         int                max_out,
                         time_t             ref_date)
{
    if (!quotes || !out || num_quotes <= 0 || max_out <= 0) return -1;

    std::vector<MarketInstrument> buf;
    buf.reserve(static_cast<size_t>(num_quotes));

    for (int i = 0; i < num_quotes; ++i) {
        MarketInstrument inst;
        if (blp_build_instrument(&inst, quotes[i].ticker,
                                  quotes[i].value, ref_date) == 0) {
            buf.push_back(inst);
        }
    }

    /* Sort by maturity (deposits first since their maturity is shortest) */
    std::sort(buf.begin(), buf.end(),
              [](const MarketInstrument &a, const MarketInstrument &b) {
                  return a.maturity < b.maturity;
              });

    int n = std::min(static_cast<int>(buf.size()), max_out);
    for (int i = 0; i < n; ++i)
        out[i] = buf[static_cast<size_t>(i)];

    return n;
}
