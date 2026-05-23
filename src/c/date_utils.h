#ifndef DATE_UTILS_H
#define DATE_UTILS_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Core date representation                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int year;
    int month;
    int day;
} DateTime;

/* ------------------------------------------------------------------ */
/*  Day count fraction conventions                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    DCF_ACT_365      = 0,  /* Actual/365 Fixed  — default; USD money market */
    DCF_ACT_360,           /* Actual/360        — SOFR, EURIBOR, FRAs       */
    DCF_30_360,            /* 30/360 Bond Basis — fixed legs of USD IRS      */
    DCF_ACT_ACT_ISDA,      /* Act/Act ISDA      — government bonds           */
    DCF_BUS_252            /* Business/252      — Brazilian DI swaps          */
} DayCountFraction;

/* ------------------------------------------------------------------ */
/*  Business day conventions                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    BDA_NONE               = 0,  /* No adjustment                            */
    BDA_FOLLOWING,               /* Next business day                         */
    BDA_MODIFIED_FOLLOWING,      /* Next BD; if month changes go back instead */
    BDA_PRECEDING                /* Previous business day                     */
} BusinessDayAdjustment;

/* ------------------------------------------------------------------ */
/*  Holiday calendar                                                   */
/* ------------------------------------------------------------------ */

#ifndef MAX_HOLIDAYS
#define MAX_HOLIDAYS 500
#endif

typedef struct {
    char name[32];                  /* e.g. "USD", "EUR", "USD|EUR"           */
    long holidays[MAX_HOLIDAYS];    /* Julian Day Numbers of holiday dates     */
    int  numHolidays;
} Calendar;

/* ------------------------------------------------------------------ */
/*  Function declarations                                              */
/* ------------------------------------------------------------------ */

/* Parsing and Julian Day conversion */
DateTime parseDateString(const char *dateStr);
long     dateToJulianDay(DateTime d);
DateTime julianDayToDate(long jdn);

/* Leap year */
int isLeapYear(int year);

/* Year fraction with explicit day count convention */
double yearFraction(DateTime start, DateTime end, DayCountFraction dcf);

/* Legacy helper (Act/365 fixed) — preserved for existing callers */
double calculateYearFraction(DateTime start, DateTime end);

/* Calendar and business day queries */
int      isWeekend(DateTime d);
int      isHoliday(DateTime d, const Calendar *cal);
int      isBusinessDay(DateTime d, const Calendar *cal);

/* Date rolling */
DateTime adjustDate(DateTime d, BusinessDayAdjustment bda, const Calendar *cal);
DateTime addDays(DateTime d, int n);
DateTime addBusinessDays(DateTime d, int n, const Calendar *cal);

/* Load holiday list from a JSON array of "YYYY-MM-DD" strings.
 * Appends to cal->holidays (does not reset existing entries).
 * Returns number of holidays added, -1 on file error. */
int loadCalendarFromFile(const char *path, const char *calName, Calendar *cal);

#endif /* DATE_UTILS_H */
