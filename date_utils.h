
#ifndef DATE_UTILS_H
#define DATE_UTILS_H

#include <stdio.h>
#include <string.h>

// Struct to store a broken-down calendar date
typedef struct {
    int year;
    int month;
    int day;
} DateTime;

DateTime parseDateString(const char* dateStr);
long dateToJulianDay(DateTime d);
double calculateYearFraction(DateTime start, DateTime end);

#endif

