#include <math.h>
#include <stdlib.h>
#include "date_utils.h"

/* ------------------------------------------------------------------ */
/*  Parsing and Julian Day conversion                                  */
/* ------------------------------------------------------------------ */

DateTime parseDateString(const char *dateStr)
{
    DateTime dt = {0, 0, 0};
    if (dateStr && strlen(dateStr) >= 10)
        sscanf(dateStr, "%4d-%2d-%2d", &dt.year, &dt.month, &dt.day);
    return dt;
}

/* Gregorian calendar → Julian Day Number */
long dateToJulianDay(DateTime d)
{
    long y = d.year, m = d.month, day = d.day;
    if (m <= 2) { y--; m += 12; }
    long a = y / 100;
    long b = a / 4;
    long c = 2 - a + b;
    long e = (long)(365.25  * (y + 4716));
    long f = (long)(30.6001 * (m + 1));
    return c + day + e + f - 1524;
}

/* Julian Day Number → Gregorian date */
DateTime julianDayToDate(long jdn)
{
    long l = jdn + 68569;
    long n = (4 * l) / 146097;
    l = l - (146097 * n + 3) / 4;
    long i = (4000 * (l + 1)) / 1461001;
    l = l - (1461 * i) / 4 + 31;
    long j = (80 * l) / 2447;
    DateTime d;
    d.day   = (int)(l - (2447 * j) / 80);
    l       = j / 11;
    d.month = (int)(j + 2 - 12 * l);
    d.year  = (int)(100 * (n - 49) + i + l);
    return d;
}

int isLeapYear(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int daysInYear(int y) { return isLeapYear(y) ? 366 : 365; }

/* ------------------------------------------------------------------ */
/*  Day count fraction helpers                                         */
/* ------------------------------------------------------------------ */

static double dcf_act365(DateTime s, DateTime e)
{
    long days = dateToJulianDay(e) - dateToJulianDay(s);
    return (double)days / 365.0;
}

static double dcf_act360(DateTime s, DateTime e)
{
    long days = dateToJulianDay(e) - dateToJulianDay(s);
    return (double)days / 360.0;
}

static double dcf_30_360(DateTime s, DateTime e)
{
    int D1 = s.day, D2 = e.day;
    int M1 = s.month, M2 = e.month;
    int Y1 = s.year,  Y2 = e.year;
    if (D1 > 30) D1 = 30;
    if (D2 > 30 && D1 == 30) D2 = 30;
    return (360.0 * (Y2 - Y1) + 30.0 * (M2 - M1) + (D2 - D1)) / 360.0;
}

static double dcf_actact_isda(DateTime s, DateTime e)
{
    if (s.year == e.year) {
        long days = dateToJulianDay(e) - dateToJulianDay(s);
        return (double)days / daysInYear(s.year);
    }
    /* Days in start year from s to Dec-31 */
    DateTime endOfStartYear = {s.year, 12, 31};
    long d1 = dateToJulianDay(endOfStartYear) - dateToJulianDay(s) + 1;
    /* Days in end year from Jan-01 to e */
    DateTime startOfEndYear = {e.year, 1, 1};
    long d2 = dateToJulianDay(e) - dateToJulianDay(startOfEndYear);
    /* Whole years in between */
    int wholeYears = e.year - s.year - 1;
    double frac = (double)d1 / daysInYear(s.year)
                + (double)d2 / daysInYear(e.year)
                + (double)wholeYears;
    return frac;
}

/* ------------------------------------------------------------------ */
/*  Calendar and business day logic                                    */
/* ------------------------------------------------------------------ */

int isWeekend(DateTime d)
{
    /* JDN mod 7: 0=Mon,1=Tue,2=Wed,3=Thu,4=Fri,5=Sat,6=Sun */
    return (dateToJulianDay(d) % 7) >= 5;
}

int isHoliday(DateTime d, const Calendar *cal)
{
    if (!cal) return 0;
    long jdn = dateToJulianDay(d);
    for (int i = 0; i < cal->numHolidays; i++)
        if (cal->holidays[i] == jdn) return 1;
    return 0;
}

int isBusinessDay(DateTime d, const Calendar *cal)
{
    return !isWeekend(d) && !isHoliday(d, cal);
}

DateTime addDays(DateTime d, int n)
{
    return julianDayToDate(dateToJulianDay(d) + n);
}

DateTime adjustDate(DateTime d, BusinessDayAdjustment bda, const Calendar *cal)
{
    if (bda == BDA_NONE) return d;

    if (bda == BDA_FOLLOWING || bda == BDA_MODIFIED_FOLLOWING) {
        DateTime adj = d;
        while (!isBusinessDay(adj, cal))
            adj = addDays(adj, 1);
        if (bda == BDA_MODIFIED_FOLLOWING && adj.month != d.month) {
            adj = d;
            while (!isBusinessDay(adj, cal))
                adj = addDays(adj, -1);
        }
        return adj;
    }

    if (bda == BDA_PRECEDING) {
        DateTime adj = d;
        while (!isBusinessDay(adj, cal))
            adj = addDays(adj, -1);
        return adj;
    }

    return d;
}

DateTime addBusinessDays(DateTime d, int n, const Calendar *cal)
{
    int step    = (n >= 0) ? 1 : -1;
    int remaining = abs(n);
    DateTime cur = d;
    while (remaining > 0) {
        cur = addDays(cur, step);
        if (isBusinessDay(cur, cal)) remaining--;
    }
    return cur;
}

/* ------------------------------------------------------------------ */
/*  Bus/252 — count business days in range, divide by 252             */
/* ------------------------------------------------------------------ */

static double dcf_bus252(DateTime s, DateTime e, const Calendar *cal)
{
    long js = dateToJulianDay(s);
    long je = dateToJulianDay(e);
    if (je <= js) return 0.0;
    int count = 0;
    for (long jdn = js + 1; jdn <= je; jdn++) {
        DateTime d = julianDayToDate(jdn);
        if (isBusinessDay(d, cal)) count++;
    }
    return (double)count / 252.0;
}

/* ------------------------------------------------------------------ */
/*  Public year fraction entry point                                   */
/* ------------------------------------------------------------------ */

double yearFraction(DateTime start, DateTime end, DayCountFraction dcf)
{
    switch (dcf) {
    case DCF_ACT_365:      return dcf_act365(start, end);
    case DCF_ACT_360:      return dcf_act360(start, end);
    case DCF_30_360:       return dcf_30_360(start, end);
    case DCF_ACT_ACT_ISDA: return dcf_actact_isda(start, end);
    case DCF_BUS_252:      return dcf_bus252(start, end, NULL);
    default:               return dcf_act365(start, end);
    }
}

/* Legacy: Act/365 for existing callers that don't pass a convention */
double calculateYearFraction(DateTime start, DateTime end)
{
    return dcf_act365(start, end);
}

/* ------------------------------------------------------------------ */
/*  Calendar loading from JSON                                         */
/* ------------------------------------------------------------------ */

int loadCalendarFromFile(const char *path, const char *calName, Calendar *cal)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /* Copy name */
    if (calName) {
        strncpy(cal->name, calName, sizeof(cal->name) - 1);
        cal->name[sizeof(cal->name) - 1] = '\0';
    }

    char line[64];
    int added = 0;
    while (fgets(line, sizeof(line), f)) {
        int y = 0, m = 0, d = 0;
        if (sscanf(line, " \"%4d-%2d-%2d\"", &y, &m, &d) == 3) {
            if (cal->numHolidays < MAX_HOLIDAYS) {
                DateTime dt = {y, m, d};
                cal->holidays[cal->numHolidays++] = dateToJulianDay(dt);
                added++;
            }
        }
    }
    fclose(f);
    return added;
}
