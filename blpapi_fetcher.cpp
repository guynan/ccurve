/*
 * blpapi_fetcher.cpp  —  Bloomberg API data fetcher implementation.
 *
 * Implements the pure-C interface declared in blpapi_fetcher.h using the
 * Bloomberg C++ SDK (BLPAPI_HOME/include, BLPAPI_HOME/lib).
 *
 * Compile:
 *   g++ -std=c++17 -fPIC -shared \
 *       -I${BLPAPI_HOME}/include \
 *       -I<project_root>          \
 *       blpapi_fetcher.cpp blpapi_data_mapper.cpp \
 *       date_utils.c              \
 *       -L${BLPAPI_HOME}/lib -lblpapi3_64 \
 *       -o libblp_fetcher.so
 *
 * All public symbols (extern "C") are stable ABI; internals are in an
 * anonymous namespace and have no guaranteed ABI across builds.
 */

/* Bloomberg C++ SDK headers */
#include <blpapi_session.h>
#include <blpapi_service.h>
#include <blpapi_request.h>
#include <blpapi_event.h>
#include <blpapi_message.h>
#include <blpapi_element.h>
#include <blpapi_name.h>
#include <blpapi_exception.h>

/* Project headers */
#include "blpapi_fetcher.h"
#include "blpapi_data_mapper.h"
#include "date_utils.h"          /* parseDateString, dateToJulianDay, etc.  */
#include "dual_curve.h"          /* MarketInstrument, InstrumentType, etc.  */

/* Standard library */
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>

/* ======================================================================== */
/*  BLPAPI Name constants — constructed once, reused                          */
/* ======================================================================== */

namespace blp_names {

using blpapi::Name;

static const Name SECURITY           ("security");
static const Name SECURITY_DATA      ("securityData");
static const Name SECURITY_ERROR     ("securityError");
static const Name FIELD_DATA         ("fieldData");
static const Name FIELD_EXCEPTIONS   ("fieldExceptions");
static const Name FIELD_ID           ("fieldId");
static const Name ERROR_INFO         ("errorInfo");
static const Name MESSAGE            ("message");
static const Name CATEGORY           ("category");
static const Name RESPONSE_ERROR     ("responseError");
static const Name DATE               ("date");
static const Name RELATIVE_DATE      ("relativeDate");
static const Name PERIODICITY_ADJUST ("periodicityAdjustment");
static const Name PERIODICITY_SEL    ("periodicitySelection");
static const Name START_DATE         ("startDate");
static const Name END_DATE           ("endDate");
static const Name SECURITIES         ("securities");
static const Name FIELDS             ("fields");

} /* namespace blp_names */

/* ======================================================================== */
/*  Internal opaque session struct                                            */
/* ======================================================================== */

struct BlpSession {
    blpapi::Session  *session;
    int               connected;   /* 1 if session started and service open */
};

/* ======================================================================== */
/*  Internal helpers (anonymous namespace, C++ linkage)                      */
/* ======================================================================== */

namespace {

/**
 * Safely copy a string into a fixed-size buffer.
 * Always NUL-terminates.
 */
inline void safe_copy(char *dst, std::size_t dst_size, const char *src) noexcept
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    std::strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/**
 * Convert a BLPAPI date element value to time_t (UTC midnight).
 *
 * Bloomberg date elements contain a blpapi::Datetime value.  We extract
 * year/month/day and compute a UTC time_t using mktime() with a zeroed
 * tm struct.  The returned value is always UTC midnight of that date.
 */
time_t blpdate_to_timet(const blpapi::Datetime &bd) noexcept
{
    struct tm t{};
    t.tm_year  = static_cast<int>(bd.year())  - 1900;
    t.tm_mon   = static_cast<int>(bd.month()) - 1;
    t.tm_mday  = static_cast<int>(bd.day());
    t.tm_hour  = 0;
    t.tm_min   = 0;
    t.tm_sec   = 0;
    t.tm_isdst = -1;
#if defined(_WIN32)
    return _mkgmtime(&t);
#else
    return timegm(&t);
#endif
}

/**
 * Attempt to parse a date string in "YYYY-MM-DD" or "MM/DD/YYYY" format
 * into a DateTime struct.  Falls back to {0,0,0} on failure.
 *
 * Bloomberg LAST_TRADEABLE_DT is typically returned as "MM/DD/YYYY" from
 * the API string representation; parseDateString() handles "YYYY-MM-DD".
 * This wrapper tries both.
 */
DateTime parse_blp_date_string(const char *s) noexcept
{
    if (!s || s[0] == '\0') return DateTime{0, 0, 0};

    /* Try ISO format first (YYYY-MM-DD) */
    DateTime dt = parseDateString(s);
    if (dt.year > 0 && dt.month > 0 && dt.day > 0)
        return dt;

    /* Try Bloomberg US format (MM/DD/YYYY) */
    int mm = 0, dd = 0, yyyy = 0;
    if (std::sscanf(s, "%2d/%2d/%4d", &mm, &dd, &yyyy) == 3 &&
        yyyy > 0 && mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31) {
        return DateTime{yyyy, mm, dd};
    }

    return DateTime{0, 0, 0};
}

/**
 * Drain events from the BLPAPI event queue until a RESPONSE event is
 * received or timeout_ms elapses.  Returns true if a RESPONSE was found,
 * false on timeout or PARTIAL_RESPONSE loop exit (caller handles partial).
 *
 * The caller supplies a functor that is invoked for each RESPONSE or
 * PARTIAL_RESPONSE message element.
 *
 * @tparam MsgHandler  Callable: void(blpapi::Message&)
 */
template<typename MsgHandler>
bool drain_until_response(blpapi::Session *session,
                          MsgHandler       handler,
                          int              timeout_ms)
{
    const int effective_timeout = (timeout_ms > 0) ? timeout_ms : 30000;

    bool done = false;
    while (!done) {
        blpapi::Event event = session->nextEvent(
            static_cast<int>(effective_timeout));

        const blpapi::Event::EventType etype = event.eventType();

        if (etype == blpapi::Event::TIMEOUT)
            return false;

        bool is_response         = (etype == blpapi::Event::RESPONSE);
        bool is_partial_response = (etype == blpapi::Event::PARTIAL_RESPONSE);

        if (is_response || is_partial_response) {
            blpapi::MessageIterator it(event);
            while (it.next()) {
                blpapi::Message msg = it.message();
                handler(msg);
            }
            if (is_response)
                done = true;   /* final fragment received */
        }
        /* Skip SESSION_STATUS, ADMIN, and other non-data events */
    }
    return true;
}

/**
 * Extract a human-readable error string from a BLPAPI error element.
 * Looks for "message" sub-element, then "category", then uses a fallback.
 */
std::string extract_error_message(const blpapi::Element &err_elem) noexcept
{
    try {
        if (err_elem.hasElement(blp_names::MESSAGE)) {
            return err_elem.getElement(blp_names::MESSAGE)
                           .getValueAsString();
        }
        if (err_elem.hasElement(blp_names::CATEGORY)) {
            return std::string("category=") +
                   err_elem.getElement(blp_names::CATEGORY)
                            .getValueAsString();
        }
    } catch (...) {}
    return "unknown BLPAPI error";
}

/* ======================================================================== */
/*  BDP response parser                                                       */
/* ======================================================================== */

/**
 * Parse a single ReferenceDataResponse message and append BlpRefResult
 * records into the results vector.
 *
 * One Message contains securityData[] where each entry holds one security
 * and its fieldData element.  We emit one result per (security, field) pair.
 */
void parse_bdp_message(const blpapi::Message         &msg,
                       const std::vector<std::string> &requested_fields,
                       std::vector<BlpRefResult>      &results)
{
    /* Top-level response error check */
    if (msg.hasElement(blp_names::RESPONSE_ERROR)) {
        BlpRefResult r{};
        safe_copy(r.ticker,  sizeof(r.ticker),  "");
        safe_copy(r.field,   sizeof(r.field),   "");
        r.ok = 0;
        const std::string emsg = extract_error_message(
            msg.getElement(blp_names::RESPONSE_ERROR));
        safe_copy(r.err, sizeof(r.err), emsg.c_str());
        results.push_back(r);
        return;
    }

    blpapi::Element sec_data_array = msg.getElement(blp_names::SECURITY_DATA);
    const std::size_t num_securities = sec_data_array.numValues();

    for (std::size_t si = 0; si < num_securities; ++si) {
        blpapi::Element sec_entry  = sec_data_array.getValueAsElement(
            static_cast<int>(si));
        const std::string ticker   = sec_entry.getElement(blp_names::SECURITY)
                                              .getValueAsString();

        /* Security-level error (e.g. security not found) */
        if (sec_entry.hasElement(blp_names::SECURITY_ERROR)) {
            for (const auto &field : requested_fields) {
                BlpRefResult r{};
                safe_copy(r.ticker, sizeof(r.ticker), ticker.c_str());
                safe_copy(r.field,  sizeof(r.field),  field.c_str());
                r.ok = 0;
                const std::string emsg = extract_error_message(
                    sec_entry.getElement(blp_names::SECURITY_ERROR));
                safe_copy(r.err, sizeof(r.err), emsg.c_str());
                results.push_back(r);
            }
            continue;
        }

        blpapi::Element field_data = sec_entry.getElement(blp_names::FIELD_DATA);

        for (const auto &field_name : requested_fields) {
            BlpRefResult r{};
            safe_copy(r.ticker, sizeof(r.ticker), ticker.c_str());
            safe_copy(r.field,  sizeof(r.field),  field_name.c_str());

            try {
                const blpapi::Name fname(field_name.c_str());

                if (!field_data.hasElement(fname)) {
                    /* Field not available for this security */
                    r.ok = 0;
                    safe_copy(r.err, sizeof(r.err),
                              "field not present in fieldData");
                    results.push_back(r);
                    continue;
                }

                blpapi::Element felem = field_data.getElement(fname);

                /*
                 * Try numeric first; fall back to string.
                 * blpapi::NotFoundException is thrown by the SDK when a
                 * conversion is not possible.
                 */
                bool got_numeric = false;
                try {
                    r.value     = felem.getValueAsFloat64();
                    got_numeric = true;
                } catch (const blpapi::NotFoundException &) {
                    /* not numeric — try string below */
                } catch (const blpapi::InvalidConversionException &) {
                    /* not numeric */
                }

                /* Always populate str_value */
                try {
                    const std::string sv = felem.getValueAsString();
                    safe_copy(r.str_value, sizeof(r.str_value), sv.c_str());
                    if (!got_numeric) {
                        /* Attempt string → double for convenience */
                        char *endp = nullptr;
                        double d = std::strtod(sv.c_str(), &endp);
                        if (endp != sv.c_str() && *endp == '\0') {
                            r.value     = d;
                            got_numeric = true;
                        }
                    }
                } catch (...) {}

                /*
                 * For date-typed elements (BLPAPI_DATATYPE_DATE), the string
                 * representation may be empty; try Datetime conversion.
                 */
                if (!got_numeric && r.str_value[0] == '\0') {
                    try {
                        blpapi::Datetime bd = felem.getValueAsDatetime();
                        char buf[32];
                        std::snprintf(buf, sizeof(buf),
                                      "%04d-%02d-%02d",
                                      static_cast<int>(bd.year()),
                                      static_cast<int>(bd.month()),
                                      static_cast<int>(bd.day()));
                        safe_copy(r.str_value, sizeof(r.str_value), buf);
                    } catch (...) {}
                }

                r.ok = 1;

            } catch (const std::exception &ex) {
                r.ok = 0;
                safe_copy(r.err, sizeof(r.err), ex.what());
            } catch (...) {
                r.ok = 0;
                safe_copy(r.err, sizeof(r.err), "unknown exception in field parse");
            }

            results.push_back(r);
        }

        /* Field-level exceptions (fields that exist but had errors) */
        if (sec_entry.hasElement(blp_names::FIELD_EXCEPTIONS)) {
            blpapi::Element fex_arr = sec_entry.getElement(
                blp_names::FIELD_EXCEPTIONS);
            const std::size_t nfe = fex_arr.numValues();
            for (std::size_t fi = 0; fi < nfe; ++fi) {
                try {
                    blpapi::Element fex = fex_arr.getValueAsElement(
                        static_cast<int>(fi));
                    const std::string fid = fex.getElement(blp_names::FIELD_ID)
                                                .getValueAsString();
                    const std::string emsg = extract_error_message(
                        fex.getElement(blp_names::ERROR_INFO));

                    /* Patch existing result for this (ticker, field) pair */
                    for (auto &r : results) {
                        if (r.ticker == ticker && r.field == fid) {
                            r.ok = 0;
                            safe_copy(r.err, sizeof(r.err), emsg.c_str());
                        }
                    }
                } catch (...) {}
            }
        }
    }
}

/* ======================================================================== */
/*  BDH response parser                                                       */
/* ======================================================================== */

/**
 * Parse a single HistoricalDataResponse message and populate the matching
 * BlpHistSeries record (matched by ticker).
 *
 * Each HistoricalDataResponse contains exactly one securityData element.
 */
void parse_bdh_message(const blpapi::Message    &msg,
                       const std::string        &field_name,
                       std::vector<BlpHistSeries> &series_vec)
{
    /* Top-level response error */
    if (msg.hasElement(blp_names::RESPONSE_ERROR)) {
        /* We can't tie the error to a ticker; mark all unprocessed */
        const std::string emsg = extract_error_message(
            msg.getElement(blp_names::RESPONSE_ERROR));
        for (auto &s : series_vec) {
            if (!s.ok) {
                safe_copy(s.err, sizeof(s.err), emsg.c_str());
            }
        }
        return;
    }

    blpapi::Element sec_data = msg.getElement(blp_names::SECURITY_DATA);

    const std::string ticker =
        sec_data.getElement(blp_names::SECURITY).getValueAsString();

    /* Find the matching series */
    BlpHistSeries *target = nullptr;
    for (auto &s : series_vec) {
        if (std::string(s.ticker) == ticker) {
            target = &s;
            break;
        }
    }
    if (!target) return;  /* unexpected ticker — skip */

    /* Security-level error */
    if (sec_data.hasElement(blp_names::SECURITY_ERROR)) {
        target->ok = 0;
        const std::string emsg = extract_error_message(
            sec_data.getElement(blp_names::SECURITY_ERROR));
        safe_copy(target->err, sizeof(target->err), emsg.c_str());
        return;
    }

    blpapi::Element field_data_arr = sec_data.getElement(blp_names::FIELD_DATA);
    const std::size_t n = field_data_arr.numValues();
    if (n == 0) {
        target->ok    = 0;
        target->count = 0;
        safe_copy(target->err, sizeof(target->err), "no data points returned");
        return;
    }

    /* Allocate point array */
    BlpHistPoint *pts = static_cast<BlpHistPoint *>(
        std::calloc(n, sizeof(BlpHistPoint)));
    if (!pts) {
        target->ok = 0;
        safe_copy(target->err, sizeof(target->err), "calloc failed");
        return;
    }

    const blpapi::Name field_blp_name(field_name.c_str());
    std::size_t written = 0;

    for (std::size_t i = 0; i < n; ++i) {
        blpapi::Element entry = field_data_arr.getValueAsElement(
            static_cast<int>(i));

        try {
            /* date field — always present */
            const blpapi::Datetime bd =
                entry.getElement(blp_names::DATE).getValueAsDatetime();
            pts[written].date = blpdate_to_timet(bd);

            /* numeric field value */
            if (entry.hasElement(field_blp_name)) {
                pts[written].value =
                    entry.getElement(field_blp_name).getValueAsFloat64();
            } else {
                pts[written].value = std::numeric_limits<double>::quiet_NaN();
            }
            ++written;
        } catch (const std::exception &) {
            /* Skip malformed entries */
        }
    }

    target->points = pts;
    target->count  = static_cast<int>(written);
    target->ok     = (written > 0) ? 1 : 0;
    if (!target->ok)
        safe_copy(target->err, sizeof(target->err), "all data points malformed");
}

} /* anonymous namespace */


/* ======================================================================== */
/*  Public C interface implementation                                         */
/* ======================================================================== */

extern "C" {

/* ------------------------------------------------------------------------ */
/*  Session lifecycle                                                         */
/* ------------------------------------------------------------------------ */

BlpSession *blp_session_create(const char *host, int port)
{
    BlpSession *s = static_cast<BlpSession *>(std::calloc(1, sizeof(BlpSession)));
    if (!s) return nullptr;

    s->session   = nullptr;
    s->connected = 0;

    try {
        blpapi::SessionOptions opts;
        opts.setServerHost(host ? host : "localhost");
        opts.setServerPort(port > 0 ? port : 8194);

        s->session = new blpapi::Session(opts);

        if (!s->session->start()) {
            /* start() returned false — connection refused or timed out */
            delete s->session;
            s->session = nullptr;
            std::free(s);
            return nullptr;
        }

        if (!s->session->openService("//blp/refdata")) {
            /* Could not open the reference data service */
            s->session->stop();
            delete s->session;
            s->session = nullptr;
            std::free(s);
            return nullptr;
        }

        s->connected = 1;

    } catch (const std::exception &) {
        if (s->session) {
            try { s->session->stop(); } catch (...) {}
            delete s->session;
        }
        std::free(s);
        return nullptr;
    } catch (...) {
        if (s->session) {
            try { s->session->stop(); } catch (...) {}
            delete s->session;
        }
        std::free(s);
        return nullptr;
    }

    return s;
}

int blp_session_connected(const BlpSession *s)
{
    return (s && s->connected) ? 1 : 0;
}

void blp_session_destroy(BlpSession *s)
{
    if (!s) return;
    if (s->session) {
        try { s->session->stop(); } catch (...) {}
        delete s->session;
        s->session = nullptr;
    }
    s->connected = 0;
    std::free(s);
}


/* ------------------------------------------------------------------------ */
/*  BDP — reference data snapshot                                             */
/* ------------------------------------------------------------------------ */

BlpRefResult *blp_fetch_bdp(BlpSession   *s,
                             const char  **tickers,
                             const char  **fields,
                             int          *out_count,
                             int           timeout_ms)
{
    if (out_count) *out_count = 0;

    if (!s || !s->connected || !s->session ||
        !tickers || !fields || !out_count)
        return nullptr;

    /* Count tickers and fields */
    int ntickers = 0;
    while (tickers[ntickers]) ++ntickers;
    int nfields = 0;
    while (fields[nfields]) ++nfields;

    if (ntickers == 0 || nfields == 0)
        return nullptr;

    std::vector<std::string> field_vec;
    field_vec.reserve(static_cast<std::size_t>(nfields));
    for (int i = 0; i < nfields; ++i)
        field_vec.emplace_back(fields[i]);

    std::vector<BlpRefResult> results;
    results.reserve(static_cast<std::size_t>(ntickers * nfields));

    try {
        blpapi::Service svc = s->session->getService("//blp/refdata");
        blpapi::Request req = svc.createRequest("ReferenceDataRequest");

        blpapi::Element sec_elem = req.getElement(blp_names::SECURITIES);
        for (int i = 0; i < ntickers; ++i)
            sec_elem.appendValue(tickers[i]);

        blpapi::Element fld_elem = req.getElement(blp_names::FIELDS);
        for (int i = 0; i < nfields; ++i)
            fld_elem.appendValue(fields[i]);

        s->session->sendRequest(req);

        const bool ok = drain_until_response(
            s->session,
            [&](blpapi::Message &msg) {
                parse_bdp_message(msg, field_vec, results);
            },
            timeout_ms);

        if (!ok) {
            /* Timeout — return what we have (may be empty) */
        }

    } catch (const std::exception &ex) {
        BlpRefResult err_r{};
        safe_copy(err_r.err, sizeof(err_r.err), ex.what());
        results.push_back(err_r);
    } catch (...) {
        BlpRefResult err_r{};
        safe_copy(err_r.err, sizeof(err_r.err), "unknown exception in blp_fetch_bdp");
        results.push_back(err_r);
    }

    if (results.empty())
        return nullptr;

    const std::size_t nres = results.size();
    BlpRefResult *arr = static_cast<BlpRefResult *>(
        std::calloc(nres, sizeof(BlpRefResult)));
    if (!arr) return nullptr;

    std::copy(results.begin(), results.end(), arr);
    *out_count = static_cast<int>(nres);
    return arr;
}


/* ------------------------------------------------------------------------ */
/*  BDH — historical time series                                              */
/* ------------------------------------------------------------------------ */

BlpHistSeries *blp_fetch_bdh(BlpSession   *s,
                              const char  **tickers,
                              const char   *field,
                              const char   *start_date,
                              const char   *end_date,
                              const char   *frequency,
                              int          *out_count,
                              int           timeout_ms)
{
    if (out_count) *out_count = 0;

    if (!s || !s->connected || !s->session ||
        !tickers || !field || !start_date || !end_date || !out_count)
        return nullptr;

    int ntickers = 0;
    while (tickers[ntickers]) ++ntickers;
    if (ntickers == 0) return nullptr;

    const std::string field_str(field);

    /* Pre-allocate result array (one entry per ticker) */
    std::vector<BlpHistSeries> series_vec(
        static_cast<std::size_t>(ntickers));

    for (int i = 0; i < ntickers; ++i) {
        std::memset(&series_vec[static_cast<std::size_t>(i)],
                    0,
                    sizeof(BlpHistSeries));
        safe_copy(series_vec[static_cast<std::size_t>(i)].ticker,
                  64, tickers[i]);
        safe_copy(series_vec[static_cast<std::size_t>(i)].field,
                  32, field);
        series_vec[static_cast<std::size_t>(i)].ok = 0;
    }

    try {
        blpapi::Service svc = s->session->getService("//blp/refdata");
        blpapi::Request req = svc.createRequest("HistoricalDataRequest");

        blpapi::Element sec_elem = req.getElement(blp_names::SECURITIES);
        for (int i = 0; i < ntickers; ++i)
            sec_elem.appendValue(tickers[i]);

        blpapi::Element fld_elem = req.getElement(blp_names::FIELDS);
        fld_elem.appendValue(field);

        req.set(blp_names::START_DATE, start_date);
        req.set(blp_names::END_DATE,   end_date);

        if (frequency && frequency[0] != '\0') {
            req.set(blp_names::PERIODICITY_ADJUST, "ACTUAL");
            req.set(blp_names::PERIODICITY_SEL,    frequency);
        }

        s->session->sendRequest(req);

        /*
         * BDH returns one PARTIAL_RESPONSE message per security, then a
         * final RESPONSE.  drain_until_response handles this correctly.
         */
        const bool ok = drain_until_response(
            s->session,
            [&](blpapi::Message &msg) {
                parse_bdh_message(msg, field_str, series_vec);
            },
            timeout_ms);

        (void)ok;

    } catch (const std::exception &ex) {
        for (auto &s_entry : series_vec) {
            s_entry.ok = 0;
            safe_copy(s_entry.err, sizeof(s_entry.err), ex.what());
        }
    } catch (...) {
        for (auto &s_entry : series_vec) {
            s_entry.ok = 0;
            safe_copy(s_entry.err, sizeof(s_entry.err),
                      "unknown exception in blp_fetch_bdh");
        }
    }

    const std::size_t nres = series_vec.size();
    BlpHistSeries *arr = static_cast<BlpHistSeries *>(
        std::calloc(nres, sizeof(BlpHistSeries)));
    if (!arr) return nullptr;

    std::copy(series_vec.begin(), series_vec.end(), arr);
    *out_count = static_cast<int>(nres);
    return arr;
}


/* ------------------------------------------------------------------------ */
/*  Memory management                                                         */
/* ------------------------------------------------------------------------ */

void blp_free(void *ptr)
{
    std::free(ptr);
}

void blp_free_hist(BlpHistSeries *series, int count)
{
    if (!series) return;
    for (int i = 0; i < count; ++i) {
        std::free(series[i].points);
        series[i].points = nullptr;
        series[i].count  = 0;
    }
    std::free(series);
}


/* ------------------------------------------------------------------------ */
/*  Convenience: fetch USD SOFR curve instruments                             */
/* ------------------------------------------------------------------------ */

int blp_fetch_curve_instruments(BlpSession       *s,
                                MarketInstrument *out,
                                int               max_instruments,
                                const char       *as_of_date)
{
    if (!s || !s->connected || !out || max_instruments <= 0 || !as_of_date)
        return -1;

    /* ------------------------------------------------------------------
     * Instrument universe definition
     * ------------------------------------------------------------------ */

    struct InstrumentSpec {
        const char *ticker;     /* Bloomberg security identifier */
        const char *rate_field; /* primary rate/price field      */
        bool        needs_ltd;  /* also request LAST_TRADEABLE_DT */
    };

    /*
     * The ordering here determines output ordering in out[]:
     *   [0]       Deposit
     *   [1..8]    SR1–SR8 futures
     *   [9..14]   Swaps  (1,2,3,5,7,10Y)
     *   [15..18]  OIS    (1,2,3,5Y)
     */
    static const InstrumentSpec SPECS[] = {
        /* Deposit */
        { "USDR3T Index",     "YLD_YTM_MID", false },
        /* SOFR futures */
        { "SR1 Comdty",       "PX_LAST",     true  },
        { "SR2 Comdty",       "PX_LAST",     true  },
        { "SR3 Comdty",       "PX_LAST",     true  },
        { "SR4 Comdty",       "PX_LAST",     true  },
        { "SR5 Comdty",       "PX_LAST",     true  },
        { "SR6 Comdty",       "PX_LAST",     true  },
        { "SR7 Comdty",       "PX_LAST",     true  },
        { "SR8 Comdty",       "PX_LAST",     true  },
        /* Vanilla USD swaps */
        { "USSW1 Curncy",     "MID",         false },
        { "USSW2 Curncy",     "MID",         false },
        { "USSW3 Curncy",     "MID",         false },
        { "USSW5 Curncy",     "MID",         false },
        { "USSW7 Curncy",     "MID",         false },
        { "USSW10 Curncy",    "MID",         false },
        /* OIS / SOFR swaps */
        { "USOSFR1 Curncy",   "MID",         false },
        { "USOSFR2 Curncy",   "MID",         false },
        { "USOSFR3 Curncy",   "MID",         false },
        { "USOSFR5 Curncy",   "MID",         false },
    };

    static constexpr int NSPECS =
        static_cast<int>(sizeof(SPECS) / sizeof(SPECS[0]));

    if (max_instruments < NSPECS) {
        /*
         * Caller buffer is too small for the full universe.
         * Proceed but cap at max_instruments to avoid overrun.
         */
    }

    const int n_to_fetch = std::min(NSPECS, max_instruments);

    /* Build NULL-terminated ticker list for blp_fetch_bdp */
    const char *ticker_ptrs[NSPECS + 1];
    for (int i = 0; i < n_to_fetch; ++i)
        ticker_ptrs[i] = SPECS[i].ticker;
    ticker_ptrs[n_to_fetch] = nullptr;

    /* Fields: rate field + LAST_TRADEABLE_DT for date lookup */
    const char *fields[] = {
        "YLD_YTM_MID",
        "PX_LAST",
        "MID",
        "LAST_TRADEABLE_DT",
        nullptr
    };

    int nresults = 0;
    BlpRefResult *raw = blp_fetch_bdp(s, ticker_ptrs, fields,
                                       &nresults, 0 /* default timeout */);
    if (!raw || nresults == 0) {
        blp_free(raw);
        return -1;
    }

    /* ------------------------------------------------------------------
     * Parse as_of_date for year-fraction calculations
     * ------------------------------------------------------------------ */
    const DateTime as_of = parseDateString(as_of_date);
    if (as_of.year == 0) {
        blp_free(raw);
        return -1;
    }

    /* ------------------------------------------------------------------
     * Build a lookup: ticker → (rate_value, ok, ltd_string)
     * ------------------------------------------------------------------ */
    struct ParsedResult {
        double rate;
        double price;          /* futures price (100 - rate) */
        char   ltd_str[64];    /* LAST_TRADEABLE_DT as string */
        bool   rate_ok;
        bool   ltd_ok;
    };

    /* Map index in SPECS → ParsedResult */
    std::vector<ParsedResult> parsed(
        static_cast<std::size_t>(n_to_fetch));

    for (int i = 0; i < n_to_fetch; ++i) {
        parsed[static_cast<std::size_t>(i)].rate    = 0.0;
        parsed[static_cast<std::size_t>(i)].price   = 0.0;
        parsed[static_cast<std::size_t>(i)].rate_ok = false;
        parsed[static_cast<std::size_t>(i)].ltd_ok  = false;
        parsed[static_cast<std::size_t>(i)].ltd_str[0] = '\0';
    }

    for (int ri = 0; ri < nresults; ++ri) {
        const BlpRefResult &r = raw[ri];

        /* Find matching spec index */
        int spec_idx = -1;
        for (int i = 0; i < n_to_fetch; ++i) {
            /* Compare without yellow key suffix: check if r.ticker starts
             * with the spec ticker up to the first space. */
            if (std::strcmp(r.ticker, SPECS[i].ticker) == 0) {
                spec_idx = i;
                break;
            }
        }
        if (spec_idx < 0 || !r.ok) continue;

        ParsedResult &pr = parsed[static_cast<std::size_t>(spec_idx)];
        const std::string field(r.field);

        if (field == "YLD_YTM_MID" || field == "MID") {
            pr.rate    = r.value / 100.0;  /* Bloomberg quotes in percent */
            pr.rate_ok = true;
        } else if (field == "PX_LAST") {
            pr.price   = r.value;
            /* Futures: price = 100 - rate*100, so rate = (100-price)/100 */
            pr.rate    = (100.0 - r.value) / 100.0;
            pr.rate_ok = true;
        } else if (field == "LAST_TRADEABLE_DT") {
            safe_copy(pr.ltd_str, sizeof(pr.ltd_str), r.str_value);
            pr.ltd_ok = (pr.ltd_str[0] != '\0');
        }
    }

    blp_free(raw);

    /* ------------------------------------------------------------------
     * Populate MarketInstrument array
     * ------------------------------------------------------------------ */
    int written = 0;

    for (int i = 0; i < n_to_fetch && written < max_instruments; ++i) {
        const InstrumentSpec &spec = SPECS[i];
        ParsedResult         &pr   = parsed[static_cast<std::size_t>(i)];

        if (!pr.rate_ok)
            continue;   /* skip instruments with no data */

        MarketInstrument &inst = out[written];
        std::memset(&inst, 0, sizeof(MarketInstrument));

        const TickerClass tc = classify_ticker(spec.ticker);

        /* Set rate / price */
        inst.rate  = pr.rate;
        inst.price = pr.price;

        switch (tc) {

        /* ---------------------------------------------------------------- */
        case TICKER_DEPOSIT:
        /* ---------------------------------------------------------------- */
            inst.type             = DEPOSIT;
            inst.startTime        = 0.0;   /* T+2 spot, simplified to 0 */
            inst.maturity         = deposit_tenor_years(spec.ticker);
            inst.paymentFrequency = 4;     /* quarterly */
            inst.fixedDcf         = DCF_ACT_360;
            inst.floatDcf         = DCF_ACT_360;
            inst.bda              = BDA_MODIFIED_FOLLOWING;
            safe_copy(inst.calendarName, sizeof(inst.calendarName), "USD");
            break;

        /* ---------------------------------------------------------------- */
        case TICKER_FUTURE:
        /* ---------------------------------------------------------------- */
        {
            inst.type  = FUTURE;
            inst.price = pr.price;

            /*
             * Maturity date: use LAST_TRADEABLE_DT if available.
             * Fall back to computing the IMM date from the contract code.
             */
            double maturity_yf = 0.0;

            if (pr.ltd_ok) {
                const DateTime ltd = parse_blp_date_string(pr.ltd_str);
                if (ltd.year > 0) {
                    maturity_yf = calculateYearFraction(as_of, ltd);
                }
            }

            if (maturity_yf <= 0.0) {
                /*
                 * LAST_TRADEABLE_DT unavailable — approximate using the IMM
                 * schedule.  SR1 expires ~3 months out, SR2 ~6 months, etc.
                 * We use the ticker digit to index quarters.
                 * Ticker: "SR1 Comdty" → digit at position 2
                 */
                const char digit_char = spec.ticker[2];
                if (digit_char >= '1' && digit_char <= '9') {
                    const int n_quarters = digit_char - '0';
                    maturity_yf = static_cast<double>(n_quarters) * 0.25;
                } else {
                    maturity_yf = 0.25;
                }
            }

            inst.maturity         = maturity_yf;
            inst.startTime        = maturity_yf - 0.25;
            if (inst.startTime < 0.0) inst.startTime = 0.0;
            inst.paymentFrequency = 4;
            inst.fixedDcf         = DCF_ACT_360;
            inst.floatDcf         = DCF_ACT_360;
            inst.bda              = BDA_MODIFIED_FOLLOWING;
            safe_copy(inst.calendarName, sizeof(inst.calendarName), "USD");
            break;
        }

        /* ---------------------------------------------------------------- */
        case TICKER_SWAP:
        /* ---------------------------------------------------------------- */
            inst.type             = SWAP;
            inst.startTime        = 0.0;
            inst.maturity         = swap_tenor_years(spec.ticker);
            inst.paymentFrequency = 2;     /* semi-annual fixed, quarterly float */
            inst.fixedDcf         = DCF_30_360;
            inst.floatDcf         = DCF_ACT_360;
            inst.bda              = BDA_MODIFIED_FOLLOWING;
            safe_copy(inst.calendarName, sizeof(inst.calendarName), "USD");
            break;

        /* ---------------------------------------------------------------- */
        case TICKER_OIS_SWAP:
        /* ---------------------------------------------------------------- */
            inst.type             = OIS_SWAP;
            inst.startTime        = 0.0;
            inst.maturity         = swap_tenor_years(spec.ticker);
            inst.paymentFrequency = 4;     /* quarterly compounded */
            inst.fixedDcf         = DCF_ACT_360;
            inst.floatDcf         = DCF_ACT_360;
            inst.bda              = BDA_MODIFIED_FOLLOWING;
            safe_copy(inst.calendarName, sizeof(inst.calendarName), "USD");
            break;

        /* ---------------------------------------------------------------- */
        default:
        /* ---------------------------------------------------------------- */
            /* Skip unrecognised tickers */
            continue;
        }

        ++written;
    }

    return written;
}


/* ======================================================================== */
/*  NZD dual-curve instrument fetch                                          */
/* ======================================================================== */

int blp_fetch_nzd_curve_instruments(BlpSession       *s,
                                     MarketInstrument *out,
                                     int               max_instruments,
                                     const char       *as_of_date)
{
    if (!s || !s->connected || !out || max_instruments <= 0 || !as_of_date)
        return -1;

    enum NzdType { NZD_OIS, NZD_DEPOSIT, NZD_FUTURE, NZD_SWAP };

    struct NzdSpec {
        const char *ticker;
        NzdType     itype;
        double      tenor_y;  /* pre-known tenor; 0 for futures (from LTD) */
    };

    /*
     * OIS instruments first (tagged OIS_SWAP), then BKBM instruments
     * (DEPOSIT, FUTURE, SWAP) in bootstrap order.  The caller splits by type.
     *
     *   [0–5]    NDSF1A–6A    OIS_SWAP  — meeting-dated NZONIA
     *   [6]      NDBB3M       DEPOSIT   — 3M BKBM bank bill
     *   [7–10]   ZB1–ZB4      FUTURE    — ASX 90-day bank bill futures
     *   [11–18]  NDSWAP3–15   SWAP      — quarterly BKBM IRS
     */
    static const NzdSpec SPECS[] = {
        { "NDSO1 Curncy",     NZD_OIS,     1.0  },
        { "NDSO2 Curncy",     NZD_OIS,     2.0  },
        { "NDSO3 Curncy",     NZD_OIS,     3.0  },
        { "NDSO4 Curncy",     NZD_OIS,     4.0  },
        { "NDSO5 Curncy",     NZD_OIS,     5.0  },
        { "NDSO6 Curncy",     NZD_OIS,     6.0  },
        { "NDBB3M Curncy",   NZD_DEPOSIT, 0.25 },
        { "ZB1 Comdty",      NZD_FUTURE,  0.0  },
        { "ZB2 Comdty",      NZD_FUTURE,  0.0  },
        { "ZB3 Comdty",      NZD_FUTURE,  0.0  },
        { "ZB4 Comdty",      NZD_FUTURE,  0.0  },
        { "NDSWAP3 Curncy",  NZD_SWAP,    3.0  },
        { "NDSWAP4 Curncy",  NZD_SWAP,    4.0  },
        { "NDSWAP5 Curncy",  NZD_SWAP,    5.0  },
        { "NDSWAP6 Curncy",  NZD_SWAP,    6.0  },
        { "NDSWAP7 Curncy",  NZD_SWAP,    7.0  },
        { "NDSWAP10 Curncy", NZD_SWAP,    10.0 },
        { "NDSWAP12 Curncy", NZD_SWAP,    12.0 },
        { "NDSWAP15 Curncy", NZD_SWAP,    15.0 },
    };
    static constexpr int NSPECS =
        static_cast<int>(sizeof(SPECS) / sizeof(SPECS[0]));

    const int n_to_fetch = std::min(NSPECS, max_instruments);

    const char *ticker_ptrs[NSPECS + 1];
    for (int i = 0; i < n_to_fetch; ++i)
        ticker_ptrs[i] = SPECS[i].ticker;
    ticker_ptrs[n_to_fetch] = nullptr;

    const char *fields[] = {
        "MID", "PX_LAST", "LAST_TRADEABLE_DT", "MATURITY", nullptr
    };

    int nresults = 0;
    BlpRefResult *raw = blp_fetch_bdp(s, ticker_ptrs, fields,
                                       &nresults, 0 /* default timeout */);
    if (!raw || nresults == 0) {
        blp_free(raw);
        return -1;
    }

    const DateTime as_of = parseDateString(as_of_date);
    if (as_of.year == 0) { blp_free(raw); return -1; }

    struct ParsedResult {
        double rate;
        double price;
        char   ltd_str[64];  /* LAST_TRADEABLE_DT for futures              */
        char   mat_str[64];  /* MATURITY for OIS; also fallback for futures */
        bool   rate_ok;
        bool   price_ok;
        bool   ltd_ok;
        bool   mat_ok;
    };

    std::vector<ParsedResult> parsed(static_cast<std::size_t>(n_to_fetch));
    for (int i = 0; i < n_to_fetch; ++i) {
        ParsedResult &pr = parsed[static_cast<std::size_t>(i)];
        pr.rate      = 0.0;
        pr.price     = 0.0;
        pr.rate_ok   = false;
        pr.price_ok  = false;
        pr.ltd_ok    = false;
        pr.mat_ok    = false;
        pr.ltd_str[0] = '\0';
        pr.mat_str[0] = '\0';
    }

    for (int ri = 0; ri < nresults; ++ri) {
        const BlpRefResult &r = raw[ri];
        int spec_idx = -1;
        for (int i = 0; i < n_to_fetch; ++i) {
            if (std::strcmp(r.ticker, SPECS[i].ticker) == 0) {
                spec_idx = i; break;
            }
        }
        if (spec_idx < 0 || !r.ok) continue;

        ParsedResult    &pr = parsed[static_cast<std::size_t>(spec_idx)];
        const std::string f = r.field;

        if (f == "MID") {
            pr.rate    = r.value / 100.0;
            pr.rate_ok = true;
        } else if (f == "PX_LAST") {
            pr.price    = r.value;
            pr.price_ok = true;
        } else if (f == "LAST_TRADEABLE_DT") {
            safe_copy(pr.ltd_str, sizeof(pr.ltd_str), r.str_value);
            pr.ltd_ok = (pr.ltd_str[0] != '\0');
        } else if (f == "MATURITY" && !pr.mat_ok) {
            safe_copy(pr.mat_str, sizeof(pr.mat_str), r.str_value);
            pr.mat_ok = (pr.mat_str[0] != '\0');
        }
    }

    blp_free(raw);

    int written = 0;

    for (int i = 0; i < n_to_fetch && written < max_instruments; ++i) {
        const NzdSpec &spec = SPECS[i];
        ParsedResult  &pr   = parsed[static_cast<std::size_t>(i)];

        MarketInstrument &inst = out[written];
        std::memset(&inst, 0, sizeof(MarketInstrument));

        inst.fixedDcf = DCF_ACT_365;
        inst.floatDcf = DCF_ACT_365;
        inst.bda      = BDA_MODIFIED_FOLLOWING;
        safe_copy(inst.calendarName, sizeof(inst.calendarName), "NZD");

        switch (spec.itype) {

        case NZD_OIS:
        {
            if (!pr.rate_ok) continue;
            /* Maturity from Bloomberg MATURITY field; fallback to tenor_y */
            double maturity_yf = 0.0;
            if (pr.mat_ok) {
                const DateTime mat = parse_blp_date_string(pr.mat_str);
                if (mat.year > 0)
                    maturity_yf = calculateYearFraction(as_of, mat);
            }
            if (maturity_yf <= 0.0)
                maturity_yf = spec.tenor_y;

            inst.type             = OIS_SWAP;
            inst.startTime        = 0.0;
            inst.maturity         = maturity_yf;
            inst.rate             = pr.rate;
            inst.paymentFrequency = 4;
            break;
        }

        case NZD_DEPOSIT:
            if (!pr.rate_ok) continue;
            inst.type             = DEPOSIT;
            inst.startTime        = 0.0;
            inst.maturity         = spec.tenor_y;
            inst.rate             = pr.rate;
            inst.paymentFrequency = 4;
            break;

        case NZD_FUTURE:
        {
            if (!pr.price_ok) continue;
            double maturity_yf = 0.0;
            if (pr.ltd_ok) {
                const DateTime ltd = parse_blp_date_string(pr.ltd_str);
                if (ltd.year > 0)
                    maturity_yf = calculateYearFraction(as_of, ltd);
            }
            if (maturity_yf <= 0.0) {
                /* Fallback: ZB1≈Q1, ZB2≈Q2 — digit at position 2 */
                const char d = spec.ticker[2];
                const int  q = (d >= '1' && d <= '9') ? (d - '0') : 1;
                maturity_yf  = static_cast<double>(q) * 0.25;
            }
            inst.type             = FUTURE;
            inst.startTime        = std::max(0.0, maturity_yf - 0.25);
            inst.maturity         = maturity_yf;
            inst.price            = pr.price;
            inst.paymentFrequency = 4;
            break;
        }

        case NZD_SWAP:
            if (!pr.rate_ok) continue;
            inst.type             = SWAP;
            inst.startTime        = 0.0;
            inst.maturity         = spec.tenor_y;
            inst.rate             = pr.rate;
            inst.paymentFrequency = 4;   /* quarterly BKBM */
            break;
        }

        ++written;
    }

    return written;
}


} /* extern "C" */
