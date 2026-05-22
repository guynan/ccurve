
#ifndef FILE_UTILS_H
#define FILE_UTILS_H

int32_t loadInstrumentsFromDatesJSON(const char *filename, const char* anchorDateStr,
								 InterestRateCurve
                                                                 *oisCurve,
                                                                 MarketInstrument
                                                                 instruments[],
                                                                 int32_t
                                                                 maxInstruments);

int32_t loadDualCurvesFromJSON(const char *filename, InterestRateCurve
*oisCurve, MarketInstrument instruments[], int32_t maxInstruments);

#endif
