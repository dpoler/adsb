// ESP32 stub — D-ATIS UI reads these globals on INFO; WiFiClientSecure
// fetch not ported yet (Pi has platform_linux/atis_linux.cpp).

#if defined(ARDUINO)
#include "atis.h"

volatile AtisStatus atis_status = ATIS_IDLE;
AtisReport atis_reports[ATIS_MAX_REPORTS] = {};
int atis_report_count = 0;

void atis_poll() {}
#endif
