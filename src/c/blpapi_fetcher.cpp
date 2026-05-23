/*
 * blpapi_fetcher.cpp — Bloomberg C++ SDK implementation of blpapi_fetcher.h.
 *
 * Compile with:
 *   g++ -std=c++17 -O2 -Wall -fPIC -shared
 *       -I src/c -I$BLPAPI_HOME/include -L$BLPAPI_HOME/lib
 *       -o libbloomberg_fetcher.so
 *       src/c/blpapi_fetcher.cpp src/c/blpapi_data_mapper.cpp
 *       src/c/dual_curve.c src/c/interp.c src/c/date_utils.c
 *       -lblpapi3 -lm
 *
 * The corresponding Python bridge (xccy_bridge.py / curve_bridge.py) does not
 * link this .so directly; it is used from C programs or a future Python layer
 * via ctypes.CDLL("libbloomberg_fetcher.so").
 */

#include "blpapi_fetcher.h"
#include "blpapi_data_mapper.h"

#include <blpapi_session.h>
#include <blpapi_eventdispatcher.h>
#include <blpapi_event.h>
#include <blpapi_message.h>
#include <blpapi_element.h>
#include <blpapi_request.h>
#include <blpapi_service.h>
#include <blpapi_name.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>

using namespace BloombergLP::blpapi;

/* ── Internal session wrapper ─────────────────────────────────────────── */

struct BlpSession {
    Session      *session   = nullptr;
    bool          connected = false;

    static constexpr const char *REF_SVC  = "//blp/refdata";
    static constexpr const char *HIST_SVC = "//blp/refdata";  /* same service */
};

static bool ensure_service(BlpSession *s, const char *svc_name) {
    if (!s || !s->connected || !s->session) return false;
    if (!s->session->openService(svc_name)) {
        fprintf(stderr, "blp_fetcher: cannot open service %s\n", svc_name);
        return false;
    }
    return true;
}

/* ── Session lifecycle ────────────────────────────────────────────────── */

extern "C"
BlpSession *blp_session_create(const char *host, int port)
{
    SessionOptions opts;
    opts.setServerHost(host ? host : "localhost");
    opts.setServerPort(port > 0 ? static_cast<unsigned short>(port) : 8194);

    auto *wrapper = new BlpSession();
    wrapper->session = new Session(opts);

    if (!wrapper->session->start()) {
        fprintf(stderr, "blp_fetcher: session start failed (%s:%d)\n", host, port);
        delete wrapper->session;
        delete wrapper;
        return nullptr;
    }
    wrapper->connected = true;
    return wrapper;
}

extern "C"
int blp_session_connected(const BlpSession *s)
{
    return (s && s->connected) ? 1 : 0;
}

extern "C"
void blp_session_destroy(BlpSession *s)
{
    if (!s) return;
    if (s->session) {
        s->session->stop();
        delete s->session;
    }
    delete s;
}

/* ── Memory ───────────────────────────────────────────────────────────── */

extern "C"
void blp_free(void *ptr) { free(ptr); }

extern "C"
void blp_free_hist(BlpHistSeries *series, int count)
{
    if (!series) return;
    for (int i = 0; i < count; ++i)
        free(series[i].points);
    free(series);
}

/* ── BDP — reference data ─────────────────────────────────────────────── */

extern "C"
BlpRefResult *blp_fetch_bdp(BlpSession   *s,
                              const char  **tickers,
                              const char  **fields,
                              int          *out_count,
                              int           timeout_ms)
{
    if (!s || !tickers || !fields || !out_count) return nullptr;
    if (!ensure_service(s, BlpSession::REF_SVC)) return nullptr;

    /* Count arrays */
    int num_tickers = 0; while (tickers[num_tickers]) ++num_tickers;
    int num_fields  = 0; while (fields[num_fields])  ++num_fields;
    int total = num_tickers * num_fields;
    if (total == 0) { *out_count = 0; return nullptr; }

    Service refSvc = s->session->getService(BlpSession::REF_SVC);
    Request  req   = refSvc.createRequest("ReferenceDataRequest");

    Element securities = req.getElement("securities");
    for (int i = 0; i < num_tickers; ++i)
        securities.appendValue(tickers[i]);

    Element flds = req.getElement("fields");
    for (int i = 0; i < num_fields; ++i)
        flds.appendValue(fields[i]);

    s->session->sendRequest(req);

    auto *results = static_cast<BlpRefResult *>(
        calloc(static_cast<size_t>(total), sizeof(BlpRefResult)));
    if (!results) return nullptr;

    /* Pre-fill ticker/field labels */
    for (int t = 0; t < num_tickers; ++t) {
        for (int f = 0; f < num_fields; ++f) {
            BlpRefResult &r = results[t * num_fields + f];
            snprintf(r.ticker, sizeof(r.ticker), "%s", tickers[t]);
            snprintf(r.field,  sizeof(r.field),  "%s", fields[f]);
        }
    }

    bool done = false;
    (void)timeout_ms;   /* Bloomberg SDK uses async events; poll until RESPONSE */
    while (!done) {
        Event ev = s->session->nextEvent(static_cast<int>(timeout_ms > 0 ? timeout_ms : 5000));
        MessageIterator msgIter(ev);
        while (msgIter.next()) {
            Message msg = msgIter.message();
            if (ev.eventType() == Event::RESPONSE ||
                ev.eventType() == Event::PARTIAL_RESPONSE) {

                Element secDataArr = msg.getElement("securityData");
                for (size_t si = 0; si < secDataArr.numValues(); ++si) {
                    Element secData = secDataArr.getValueAsElement(si);
                    std::string ticker = secData.getElementAsString("security");

                    /* Match ticker to row index */
                    int ti = -1;
                    for (int t = 0; t < num_tickers; ++t) {
                        if (ticker == tickers[t]) { ti = t; break; }
                    }
                    if (ti < 0) continue;

                    if (secData.hasElement("securityError")) {
                        Element err = secData.getElement("securityError");
                        for (int f = 0; f < num_fields; ++f) {
                            BlpRefResult &r = results[ti * num_fields + f];
                            snprintf(r.err, sizeof(r.err), "%s",
                                     err.getElementAsString("message"));
                        }
                        continue;
                    }

                    Element fieldData = secData.getElement("fieldData");
                    for (int f = 0; f < num_fields; ++f) {
                        BlpRefResult &r = results[ti * num_fields + f];
                        if (fieldData.hasElement(fields[f])) {
                            r.value = fieldData.getElementAsFloat64(fields[f]);
                            r.ok    = 1;
                        } else {
                            snprintf(r.err, sizeof(r.err), "field not found: %s", fields[f]);
                        }
                    }
                }
            }
        }
        if (ev.eventType() == Event::RESPONSE) done = true;
    }

    *out_count = total;
    return results;
}

/* ── BDH — historical data ────────────────────────────────────────────── */

extern "C"
BlpHistSeries *blp_fetch_bdh(BlpSession   *s,
                               const char  **tickers,
                               const char   *field,
                               const char   *start_date,
                               const char   *end_date,
                               const char   *frequency,
                               int          *out_count,
                               int           timeout_ms)
{
    if (!s || !tickers || !field || !start_date || !end_date || !out_count)
        return nullptr;
    if (!ensure_service(s, BlpSession::HIST_SVC)) return nullptr;

    int num_tickers = 0; while (tickers[num_tickers]) ++num_tickers;
    if (num_tickers == 0) { *out_count = 0; return nullptr; }

    Service histSvc = s->session->getService(BlpSession::HIST_SVC);
    Request  req    = histSvc.createRequest("HistoricalDataRequest");

    Element securities = req.getElement("securities");
    for (int i = 0; i < num_tickers; ++i)
        securities.appendValue(tickers[i]);

    req.getElement("fields").appendValue(field);
    req.set("startDate",       start_date);
    req.set("endDate",         end_date);
    req.set("periodicitySelection", frequency ? frequency : "DAILY");
    req.set("nonTradingDayFillOption", "PREVIOUS_VALUE");

    s->session->sendRequest(req);

    auto *series = static_cast<BlpHistSeries *>(
        calloc(static_cast<size_t>(num_tickers), sizeof(BlpHistSeries)));
    if (!series) return nullptr;

    for (int i = 0; i < num_tickers; ++i) {
        snprintf(series[i].ticker, sizeof(series[i].ticker), "%s", tickers[i]);
        snprintf(series[i].field,  sizeof(series[i].field),  "%s", field);
    }

    bool done = false;
    (void)timeout_ms;
    while (!done) {
        Event ev = s->session->nextEvent(timeout_ms > 0 ? timeout_ms : 5000);
        MessageIterator msgIter(ev);
        while (msgIter.next()) {
            Message msg = msgIter.message();
            if (ev.eventType() != Event::RESPONSE &&
                ev.eventType() != Event::PARTIAL_RESPONSE) continue;

            Element secData = msg.getElement("securityData");
            std::string ticker = secData.getElementAsString("security");

            int ti = -1;
            for (int t = 0; t < num_tickers; ++t) {
                if (ticker == tickers[t]) { ti = t; break; }
            }
            if (ti < 0) continue;

            if (secData.hasElement("securityError")) {
                Element err = secData.getElement("securityError");
                snprintf(series[ti].err, sizeof(series[ti].err), "%s",
                         err.getElementAsString("message"));
                continue;
            }

            Element fieldData = secData.getElement("fieldData");
            int n = static_cast<int>(fieldData.numValues());
            series[ti].points = static_cast<BlpHistPoint *>(
                malloc(static_cast<size_t>(n) * sizeof(BlpHistPoint)));
            series[ti].count = n;
            series[ti].ok    = 1;

            for (int k = 0; k < n; ++k) {
                Element row = fieldData.getValueAsElement(static_cast<size_t>(k));
                /* date field is a Datetime object */
                Datetime dt = row.getElementAsDatetime("date");
                struct tm tm_val = {};
                tm_val.tm_year = dt.year()  - 1900;
                tm_val.tm_mon  = dt.month() - 1;
                tm_val.tm_mday = dt.day();
                series[ti].points[k].date  = mktime(&tm_val);
                series[ti].points[k].value = row.getElementAsFloat64(field);
            }
        }
        if (ev.eventType() == Event::RESPONSE) done = true;
    }

    *out_count = num_tickers;
    return series;
}

/* ── Convenience: fetch all curve instruments ─────────────────────────── */

/*
 * USD curve ticker universe.
 * Deposits: short-end anchor rates.
 * Futures : SOFR futures (SR) preferred; Eurodollar (ED) as fallback.
 * Swaps   : fixed-vs-SOFR standard tenors.
 * OIS     : SOFR OIS tenors for the discount curve.
 */
static const char *USD_TICKERS[] = {
    /* deposits */
    "USDR3T Index",     /* 3M USD deposit (T+2 settle) */
    "USDR1Z Index",     /* O/N deposit */
    /* SOFR futures SR1–SR8 */
    "SR1 Comdty", "SR2 Comdty", "SR3 Comdty", "SR4 Comdty",
    "SR5 Comdty", "SR6 Comdty", "SR7 Comdty", "SR8 Comdty",
    /* SOFR OIS */
    "USOSFR1 Curncy", "USOSFR2 Curncy", "USOSFR3 Curncy", "USOSFR5 Curncy",
    /* par swaps (fixed vs 3M SOFR) */
    "USSW1 Curncy", "USSW2 Curncy", "USSW3 Curncy",
    "USSW5 Curncy", "USSW7 Curncy", "USSW10 Curncy",
    nullptr
};

static const char *USD_FIELDS_DEPOSITS[] = { "YLD_MID",  nullptr };
static const char *USD_FIELDS_FUTURES[]  = { "PX_LAST",  nullptr };
static const char *USD_FIELDS_SWAPS[]    = { "MID",      nullptr };

extern "C"
int blp_fetch_curve_instruments(BlpSession       *s,
                                 MarketInstrument *out,
                                 int               max_instruments,
                                 const char       *as_of_date)
{
    if (!s || !out || max_instruments <= 0) return -1;

    /* Derive ref_date from as_of_date ("YYYY-MM-DD") */
    time_t ref_date = time(nullptr);
    if (as_of_date && strlen(as_of_date) >= 10) {
        struct tm tm_val = {};
        if (sscanf(as_of_date, "%d-%d-%d",
                   &tm_val.tm_year, &tm_val.tm_mon, &tm_val.tm_mday) == 3) {
            tm_val.tm_year -= 1900;
            tm_val.tm_mon  -= 1;
            ref_date = mktime(&tm_val);
        }
    }

    /* ── 1. Fetch all tickers with PX_LAST/MID/YLD_MID ── */
    /* Use PX_LAST as a catch-all; the mapper knows which field to use per type */
    const char *all_fields[] = { "PX_LAST", "MID", "YLD_MID", nullptr };
    int total = 0;
    BlpRefResult *raw = blp_fetch_bdp(s, USD_TICKERS, all_fields, &total,
                                       10000 /* 10 s timeout */);
    if (!raw || total <= 0) {
        blp_free(raw);
        return -1;
    }

    /* Count tickers */
    int num_tickers = 0;
    while (USD_TICKERS[num_tickers]) ++num_tickers;
    int num_fields = 3; /* PX_LAST, MID, YLD_MID */

    /* ── 2. Build BlpRawQuote array ── */
    std::vector<BlpRawQuote> quotes;
    quotes.reserve(static_cast<size_t>(num_tickers));

    for (int ti = 0; ti < num_tickers; ++ti) {
        const char *ticker = USD_TICKERS[ti];
        int inst_type = blp_ticker_type(ticker);
        if (inst_type < 0) continue;

        /* Pick the right field index based on instrument type */
        const char *preferred_field;
        if (inst_type == static_cast<int>(DEPOSIT))
            preferred_field = "YLD_MID";
        else if (inst_type == static_cast<int>(FUTURE))
            preferred_field = "PX_LAST";
        else
            preferred_field = "MID";  /* SWAP, OIS_SWAP */

        double best_value = 0.0;
        bool   found      = false;
        for (int fi = 0; fi < num_fields && !found; ++fi) {
            BlpRefResult &r = raw[ti * num_fields + fi];
            if (r.ok && strcmp(r.field, preferred_field) == 0) {
                best_value = r.value;
                found = true;
            }
        }
        /* Fallback: any ok field */
        for (int fi = 0; fi < num_fields && !found; ++fi) {
            BlpRefResult &r = raw[ti * num_fields + fi];
            if (r.ok) { best_value = r.value; found = true; }
        }

        if (!found) continue;

        BlpRawQuote q;
        snprintf(q.ticker, sizeof(q.ticker), "%s", ticker);
        q.value = best_value;
        quotes.push_back(q);
    }

    blp_free(raw);

    /* ── 3. Map raw quotes to MarketInstrument[] ── */
    int written = blp_map_instruments(quotes.data(),
                                       static_cast<int>(quotes.size()),
                                       out, max_instruments, ref_date);
    return written;
}
