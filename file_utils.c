

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
			char *typeKey = strstr(line, "\"type\"");
			char *startKey = strstr(line, "\"startDate\"");
			char *matKey = strstr(line, "\"maturityDate\"");
			char *rateKey = strstr(line, "\"rate\"");
			char *priceKey = strstr(line, "\"price\"");
			char *freqKey = strstr(line, "\"paymentFrequency\"");

			// Incremental key extractions (handles both spaced and dense JSON variants)
			if (typeKey)  { 
				if (sscanf(typeKey, "\"type\" : \"%15[^\"]\"", typeStr) != 1) {
					sscanf(typeKey, "\"type\":\"%15[^\"]\"", typeStr);
				}
			}
			if (startKey) { 
				if (sscanf(startKey, "\"startDate\" : \"%10[^\"]\"", startExStr) != 1) {
					sscanf(startKey, "\"startDate\":\"%10[^\"]\"", startExStr);
				}
			}
			if (matKey)   { 
				if (sscanf(matKey, "\"maturityDate\" : \"%10[^\"]\"", matExStr) != 1) {
					sscanf(matKey, "\"maturityDate\":\"%10[^\"]\"", matExStr);
				}
			}
			
			if (rateKey)  { char *colon = strchr(rateKey, ':'); if (colon) rate = strtod(colon + 1, NULL); }
			if (priceKey) { char *colon = strchr(priceKey, ':'); if (colon) price = strtod(colon + 1, NULL); }
			if (freqKey)  { char *colon = strchr(freqKey, ':'); if (colon) frequency = (int)strtol(colon + 1, NULL, 10); }

			// Evaluate, parse, and append data structures safely at block boundary
			if (strstr(line, "}") && strlen(typeStr) > 0 && strlen(matExStr) > 0) {
				
				int32_t validInstrument = 0;
				if (strcmp(typeStr, "DEPOSIT") == 0) {
					instruments[instCount].type = DEPOSIT;
					validInstrument = 1;
				} else if (strcmp(typeStr, "FUTURE") == 0) {
					instruments[instCount].type = FUTURE;
					validInstrument = 1;
				} else if (strcmp(typeStr, "SWAP") == 0) {
					instruments[instCount].type = SWAP;
					validInstrument = 1;
				}

				if (validInstrument && instCount < maxInstruments) {
					DateTime startEvent = parseDateString(startExStr);
					DateTime matEvent = parseDateString(matExStr);

					instruments[instCount].startTime = calculateYearFraction(anchorDate, startEvent);
					instruments[instCount].maturity = calculateYearFraction(anchorDate, matEvent);
					instruments[instCount].rate = rate;
					instruments[instCount].price = price;
					instruments[instCount].paymentFrequency = frequency;
					instCount++;
				}

				// Clean buffers immediately so upcoming elements start from blank sheets
				typeStr[0] = '\0';
				startExStr[0] = '\0';
				matExStr[0] = '\0';
				rate = -1.0;
				price = -1.0;
				frequency = -1;
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
				if (strcmp(typeStr, "DEPOSIT") == 0) instruments[instCount].type = DEPOSIT;
				else if (strcmp(typeStr, "FUTURE") == 0) instruments[instCount].type = FUTURE;
				else if (strcmp(typeStr, "SWAP") == 0) instruments[instCount].type = SWAP;
				else { maturity = -1.0; continue; }

				instruments[instCount].startTime = startTime;
				instruments[instCount].maturity = maturity;
				instruments[instCount].rate = rate;
				instruments[instCount].price = price;
				instruments[instCount].paymentFrequency = frequency;
				instCount++;

				typeStr[0] = '\0'; startTime = 0.0; maturity = -1.0; rate = -1.0; price = -1.0; frequency = -1;
			}
		}
	}
	oisCurve->numNodes = oisCount;
	fclose(file);
	return instCount;
}

