#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void setupMulticast();
void pollSystemStats();
void multicastNmeaString(const char *nmea);
void addRawGpsChar(char c);
void pollRawMulticast();

#ifdef __cplusplus
}
#endif
