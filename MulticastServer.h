#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void setup_multicast();
void poll_system_stats();
void multicast_nmea_string(const char *nmea);
void send_pending_nmea_string();
void add_raw_gps_char(char c);
void poll_raw_multicast();

#ifdef __cplusplus
}
#endif
