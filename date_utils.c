
#include "date_utils.h"


// Converts an ISO date string "YYYY-MM-DD" into a DateTime struct
DateTime parseDateString(const char* dateStr) {
    DateTime dt = {0, 0, 0};
    if (dateStr && strlen(dateStr) >= 10) {
        sscanf(dateStr, "%4d-%2d-%2d", &dt.year, &dt.month, &dt.day);
    }
    return dt;
}

// Algorithm to convert a Gregorian date to a Julian Day Number (JDN)
long dateToJulianDay(DateTime d) {
    long y = d.year;
    long m = d.month;
    long day = d.day;

    // Adjustments for internal monthly integer alignments
    if (m <= 2) {
        y--;
        m += 12;
    }

    long a = y / 100;
    long b = a / 4;
    long c = 2 - a + b;
    long e = (long)(365.25 * (y + 4716));
    long f = (long)(30.6001 * (m + 1));
    
    return c + day + e + f - 1524;
}

// Computes year fraction between two dates using Actual/365 day count convention
double calculateYearFraction(DateTime start, DateTime end) {
    long startJDN = dateToJulianDay(start);
    long endJDN = dateToJulianDay(end);
    long daysBetween = endJDN - startJDN;
    
    // Actual / 365 baseline conversion mapping rule
    return (double)daysBetween / 365.0;
}


