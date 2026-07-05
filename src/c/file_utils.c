

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

#include "dual_curve.h"
#include "date_utils.h"
#include "file_utils.h"
// ==========================================
// 7. Streaming Dual Curve JSON Tokenizer
// ==========================================


int32_t loadInstrumentsFromDatesJSON(const char *filename, const char* anchorDateStr,
								 InterestRateCurve *oisCurve, MarketInstrument instruments[], int32_t maxInstruments) {
	FILE *file = fopen(filename, "r");
	if (!file) {
		printf("Error: Could not open market layout data file %s\n", filename);
		return -1;
	}

	DateTime anchorDate = parseDateString(anchorDateStr);
	char line[256];
	int32_t instCount = 0;
	int32_t oisCount = 0;
	int32_t cbCount = 0;

	// Explicit state tracking for sections
	int32_t readingMeetings = 0;
	int32_t readingOis = 0;
	int32_t readingMarket = 0;

	char typeStr[32] = "";
	char startExStr[32] = "";
	char matExStr[32] = "";
	char oisDateStr[32] = "";
	char meetingDateStr[32] = "";
	
	double rate = -1.0, price = -1.0, targetRateInput = -1.0;
	int32_t frequency = -1;

	while (fgets(line, sizeof(line), file)) {
		
		// ==========================================
		// 1. Section Header Detection
		// ==========================================
		if (strstr(line, "\"meeting_schedule\"")) {
			readingMeetings = 1;
			readingOis = 0;
			readingMarket = 0;
			continue;
		}
		if (strstr(line, "\"ois_curve\"")) {
			readingOis = 1;
			readingMeetings = 0;
			readingMarket = 0;
			continue;
		}
		if (strstr(line, "\"market_data\"")) { 
			readingMarket = 1;
			readingOis = 0;
			readingMeetings = 0;
			continue;
		}

		// ==========================================
		// 2. Central Bank Meetings Parser
		// ==========================================
		if (readingMeetings) {
			char *dateKey = strstr(line, "\"date\"");
			char *rateKey = strstr(line, "\"target_rate\"");
			
			if (dateKey) {
				if (sscanf(dateKey, "\"date\" : \"%10[^\"]\"", meetingDateStr) != 1) {
					sscanf(dateKey, "\"date\":\"%10[^\"]\"", meetingDateStr);
				}
			}
			if (rateKey) {
				char *colon = strchr(rateKey, ':');
				if (colon) targetRateInput = strtod(colon + 1, NULL);
			}
			if (strstr(line, "}") && cbCount < MAX_NODES && strlen(meetingDateStr) > 0) {
				DateTime mtDate = parseDateString(meetingDateStr);
				double t = calculateYearFraction(anchorDate, mtDate);
				
				oisCurve->cbSchedule.meetingTimes[cbCount] = t;
				oisCurve->cbSchedule.targetRates[cbCount] = targetRateInput;
				cbCount++;
				
				meetingDateStr[0] = '\0';
				targetRateInput = -1.0;
			}
		}
		// ==========================================
		// 3. OIS Term Curve Nodes Parser
		// ==========================================
		else if (readingOis) {
			char *dateKey = strstr(line, "\"date\"");
			char *rateKey = strstr(line, "\"rate\"");
			
			if (dateKey) {
				if (sscanf(dateKey, "\"date\" : \"%10[^\"]\"", oisDateStr) != 1) {
					sscanf(dateKey, "\"date\":\"%10[^\"]\"", oisDateStr);
				}
			}
			if (rateKey) {
				char *colon = strchr(rateKey, ':');
				if (colon) rate = strtod(colon + 1, NULL);
			}
			if (strstr(line, "}") && oisCount < MAX_NODES && strlen(oisDateStr) > 0) {
				DateTime targetOisDate = parseDateString(oisDateStr);
				double t = calculateYearFraction(anchorDate, targetOisDate);

				oisCurve->times[oisCount] = t;
				oisCurve->rates[oisCount] = rate;
				oisCurve->dfs[oisCount] = exp(-rate * t);
				oisCount++;

				oisDateStr[0] = '\0';
				rate = -1.0;
			}
		}
		// ==========================================
		// 4. Market Instruments Array Parser
		// ==========================================
		else if (readingMarket) {
			char *typeKey     = strstr(line, "\"type\"");
			char *startKey    = strstr(line, "\"startDate\"");
			char *matKey      = strstr(line, "\"maturityDate\"");
			char *rateKey     = strstr(line, "\"rate\"");
			char *priceKey    = strstr(line, "\"price\"");
			char *freqKey     = strstr(line, "\"paymentFrequency\"");
			char *fixedDcfKey = strstr(line, "\"fixedDcf\"");
			char *floatDcfKey = strstr(line, "\"floatDcf\"");
			char *bdaKey      = strstr(line, "\"bda\"");
			char *calKey      = strstr(line, "\"calendar\"");

			if (typeKey)  {
				if (sscanf(typeKey, "\"type\" : \"%15[^\"]\"", typeStr) != 1)
					sscanf(typeKey, "\"type\":\"%15[^\"]\"", typeStr);
			}
			if (startKey) {
				if (sscanf(startKey, "\"startDate\" : \"%10[^\"]\"", startExStr) != 1)
					sscanf(startKey, "\"startDate\":\"%10[^\"]\"", startExStr);
			}
			if (matKey) {
				if (sscanf(matKey, "\"maturityDate\" : \"%10[^\"]\"", matExStr) != 1)
					sscanf(matKey, "\"maturityDate\":\"%10[^\"]\"", matExStr);
			}
			if (rateKey)     { char *c = strchr(rateKey,  ':'); if (c) rate      = strtod(c + 1, NULL); }
			if (priceKey)    { char *c = strchr(priceKey, ':'); if (c) price     = strtod(c + 1, NULL); }
			if (freqKey)     { char *c = strchr(freqKey,  ':'); if (c) frequency = (int)strtol(c + 1, NULL, 10); }

			/* Optional convention fields */
			char dcfStr[32] = "", bdaStr[32] = "", calStr[32] = "";
			if (fixedDcfKey) {
				if (sscanf(fixedDcfKey, "\"fixedDcf\" : \"%31[^\"]\"", dcfStr) != 1)
					sscanf(fixedDcfKey, "\"fixedDcf\":\"%31[^\"]\"", dcfStr);
			}
			if (floatDcfKey) {
				char fdc[32] = "";
				if (sscanf(floatDcfKey, "\"floatDcf\" : \"%31[^\"]\"", fdc) != 1)
					sscanf(floatDcfKey, "\"floatDcf\":\"%31[^\"]\"", fdc);
				/* stored below per instrument */
				strncpy(dcfStr, fdc, sizeof(dcfStr) - 1);
				dcfStr[sizeof(dcfStr) - 1] = '\0';
			}
			if (bdaKey) {
				if (sscanf(bdaKey, "\"bda\" : \"%31[^\"]\"", bdaStr) != 1)
					sscanf(bdaKey, "\"bda\":\"%31[^\"]\"", bdaStr);
			}
			if (calKey) {
				if (sscanf(calKey, "\"calendar\" : \"%31[^\"]\"", calStr) != 1)
					sscanf(calKey, "\"calendar\":\"%31[^\"]\"", calStr);
			}

			if (strstr(line, "}") && strlen(typeStr) > 0 && strlen(matExStr) > 0) {
				InstrumentType t = -1;
				if      (strcmp(typeStr, "DEPOSIT")  == 0) t = DEPOSIT;
				else if (strcmp(typeStr, "FUTURE")   == 0) t = FUTURE;
				else if (strcmp(typeStr, "SWAP")     == 0) t = SWAP;
				else if (strcmp(typeStr, "OIS_SWAP") == 0) t = OIS_SWAP;

				if ((int)t >= 0 && instCount < maxInstruments) {
					DateTime startEvent = parseDateString(startExStr);
					DateTime matEvent   = parseDateString(matExStr);

					MarketInstrument *ins = &instruments[instCount];
					memset(ins, 0, sizeof(*ins));
					ins->type      = t;
					ins->startTime = calculateYearFraction(anchorDate, startEvent);
					ins->maturity  = calculateYearFraction(anchorDate, matEvent);

					/* Map DCF string to enum (default: Act/365) */
					DayCountFraction dcf = DCF_ACT_365;
					if      (strcmp(dcfStr, "ACT_360")      == 0) dcf = DCF_ACT_360;
					else if (strcmp(dcfStr, "30_360")       == 0) dcf = DCF_30_360;
					else if (strcmp(dcfStr, "ACT_ACT_ISDA") == 0) dcf = DCF_ACT_ACT_ISDA;
					else if (strcmp(dcfStr, "BUS_252")      == 0) dcf = DCF_BUS_252;

					/* Map BDA string to enum (default: none) */
					BusinessDayAdjustment bda = BDA_NONE;
					if      (strcmp(bdaStr, "FOLLOWING")          == 0) bda = BDA_FOLLOWING;
					else if (strcmp(bdaStr, "MODIFIED_FOLLOWING") == 0) bda = BDA_MODIFIED_FOLLOWING;
					else if (strcmp(bdaStr, "PRECEDING")          == 0) bda = BDA_PRECEDING;

					switch (t) {
					case DEPOSIT:
						ins->spec.deposit.rate = rate;
						ins->spec.deposit.dcf  = dcf;
						ins->spec.deposit.bda  = bda;
						strncpy(ins->spec.deposit.calendarName, calStr,
						        sizeof(ins->spec.deposit.calendarName) - 1);
						break;
					case FUTURE:
						ins->spec.future.price = price;
						ins->spec.future.dcf   = dcf;
						strncpy(ins->spec.future.calendarName, calStr,
						        sizeof(ins->spec.future.calendarName) - 1);
						break;
					case SWAP:
					case OIS_SWAP:
						ins->spec.swap.rate             = rate;
						ins->spec.swap.paymentFrequency = frequency;
						ins->spec.swap.fixedDcf         = dcf;
						ins->spec.swap.floatDcf         = dcf;
						ins->spec.swap.bda              = bda;
						strncpy(ins->spec.swap.calendarName, calStr,
						        sizeof(ins->spec.swap.calendarName) - 1);
						break;
					default: break;
					}

					instCount++;
				}

				typeStr[0] = '\0'; startExStr[0] = '\0'; matExStr[0] = '\0';
				rate = -1.0; price = -1.0; frequency = -1;
			}
		}
	}

	oisCurve->cbSchedule.numMeetings = cbCount;
	oisCurve->numNodes = oisCount;
	
	fclose(file);
	return instCount;
}


int32_t loadDualCurvesFromJSON(const char *filename, InterestRateCurve *oisCurve, MarketInstrument instruments[], int32_t maxInstruments) {
	FILE *file = fopen(filename, "r");
	if (!file) return -1;

	char line[256];
	int32_t instCount = 0;
	int32_t oisCount = 0;
	int32_t readingOis = 0;

	char typeStr[32] = "";
	double startTime = 0.0, maturity = -1.0, rate = -1.0, price = -1.0;
	int32_t frequency = -1;

	while (fgets(line, sizeof(line), file)) {
		if (strstr(line, "\"ois_curve\"")) { readingOis = 1; continue; }
		if (strstr(line, "\"market_data\"")) { readingOis = 0; continue; }

		if (readingOis) {
			char *timeKey = strstr(line, "\"time\"");
			char *rateKey = strstr(line, "\"rate\"");
			if (timeKey) {
				char *colon = strchr(timeKey, ':');
				if (colon) oisCurve->times[oisCount] = strtod(colon + 1, NULL);
			}
			if (rateKey) {
				char *colon = strchr(rateKey, ':');
				if (colon) {
					double r = strtod(colon + 1, NULL);
					oisCurve->rates[oisCount] = r;
					oisCurve->dfs[oisCount] = exp(-r * oisCurve->times[oisCount]);
				}
			}
			if (strstr(line, "}") && oisCount < MAX_NODES) {
				oisCount++;
			}
		} else {
			char *typeKey = strstr(line, "\"type\"");
			char *startKey = strstr(line, "\"startTime\"");
			char *matKey = strstr(line, "\"maturity\"");
			char *rateKey = strstr(line, "\"rate\"");
			char *priceKey = strstr(line, "\"price\"");
			char *freqKey = strstr(line, "\"paymentFrequency\"");

			if (typeKey) {
				char *quote = strchr(typeKey + 6, '"');
				if (quote) {
					char *endQuote = strchr(quote + 1, '"');
					if (endQuote) {
						ptrdiff_t len = endQuote - (quote + 1);
						if (len > 31) len = 31;
						strncpy(typeStr, quote + 1, len);
						typeStr[len] = '\0';
					}
				}
			}
			if (startKey) { char *colon = strchr(startKey, ':'); if (colon) startTime = strtod(colon + 1, NULL); }
			if (matKey)   { char *colon = strchr(matKey, ':'); if (colon) maturity = strtod(colon + 1, NULL); }
			if (rateKey)  { char *colon = strchr(rateKey, ':'); if (colon) rate = strtod(colon + 1, NULL); }
			if (priceKey) { char *colon = strchr(priceKey, ':'); if (colon) price = strtod(colon + 1, NULL); }
			if (freqKey)  { char *colon = strchr(freqKey, ':'); if (colon) frequency = (int)strtol(colon + 1, NULL, 10); }

			if (strstr(line, "}") && maturity > 0.0 && instCount < maxInstruments) {
				MarketInstrument *ins = &instruments[instCount];
				memset(ins, 0, sizeof(*ins));

				if      (strcmp(typeStr, "DEPOSIT") == 0) ins->type = DEPOSIT;
				else if (strcmp(typeStr, "FUTURE")  == 0) ins->type = FUTURE;
				else if (strcmp(typeStr, "SWAP")    == 0) ins->type = SWAP;
				else { maturity = -1.0; continue; }

				ins->startTime = startTime;
				ins->maturity  = maturity;
				switch (ins->type) {
				case DEPOSIT: ins->spec.deposit.rate = rate;   break;
				case FUTURE:  ins->spec.future.price = price;  break;
				case SWAP:
				case OIS_SWAP:
					ins->spec.swap.rate             = rate;
					ins->spec.swap.paymentFrequency = frequency;
					break;
				default: break;
				}
				instCount++;

				typeStr[0] = '\0'; startTime = 0.0; maturity = -1.0; rate = -1.0; price = -1.0; frequency = -1;
			}
		}
	}
	oisCurve->numNodes = oisCount;
	fclose(file);
	return instCount;
}

