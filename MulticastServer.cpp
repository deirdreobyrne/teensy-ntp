#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "DateTime.h"
#include "GPS.h"
#include "WebContent.h"
#include "lwipopts.h"
#include "lwip_t41.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"
#include "lwip/udp.h"
// core_pins.h for tempmonGetTemp
#include "core_pins.h"
// #include "MulticastServer.h"
#include "settings.h"

// WAS once every 5 minnutes - when all I was sending was the temperature
#define SYSTEM_STATS_PERIOD_MS 59000
#define RAW_GPS_BUFFER_SIZE 1200
#define RAW_GPS_MAX_QUIET_MS 250
#define SYSTEM_STATS_STRLEN 1400

#define PERIOD_EXPIRED(nowMillis,startMillis,period) ((nowMillis - startMillis) >= period)

ip4_addr_t systemStatsAddr, nmeaAddr, rawAddr;
uint32_t systemStatsTime;
#ifdef GPS_SECOND_SERIAL_PORT
char rawGpsBuffer[RAW_GPS_BUFFER_SIZE];
int rawGpsBufferLen = 0;
uint32_t lastRawSendTime;
#endif
char *nmeaString = 0;

void setup_multicast() {
  IP4_ADDR(&systemStatsAddr, 239,0,0,1);
  IP4_ADDR(&nmeaAddr, 239,0,0,2);
#ifdef GPS_SECOND_SERIAL_PORT
  IP4_ADDR(&rawAddr, 239,0,0,3);
  lastRawSendTime = millis();
#endif
  systemStatsTime = millis() - SYSTEM_STATS_PERIOD_MS;
}

void compile_system_stats_json(char *json_buf, int json_buf_len) {
  int len = snprintf(json_buf, json_buf_len, 
    "{\"topic\":\"teensy-gps\", \"payload\":{"
    "\"temp\":%.1f, \"ppsToGPS\": %lu, \"ppsMs\": %lu, \"curMs\": %lu, \"gpsT\": %lu, \"counterPPS\": %lu, \"offset\": %.9f, \"pidD\": %.9f, \"dChiSq\": %.9f, \"clockPpb\": %ld,",
    tempmonGetTemp(),
    webcontent.ppsToGPS,
    webcontent.ppsMillis,
    millis(),
    webcontent.gpstime,
    webcontent.counterPPS,
    webcontent.offsetHuman,
    webcontent.pidD,
    webcontent.dChiSq,
    webcontent.clockPpb);
  if (len >= json_buf_len) {
    json_buf[json_buf_len-1]='\0';
    return;
  }
  len += snprintf(json_buf + len, json_buf_len - len,
    "\"lockStatus\": %u, \"strongSig\": %lu, \"weakSig\": %lu, \"noSig\": %lu, \"gpsCap\": %lu, \"pdop\": %.1f, \"hdop\": %.1f, \"vdop\": %.1f, \"sats\": [",
    gps.lockStatus(),
    gps.strongSignals(),
    gps.weakSignals(),
    gps.noSignals(),
    gps.capturedAt(),
    gps.getPdop(),
    gps.getHdop(),
    gps.getVdop()
    );
  if (len >= json_buf_len) {
    json_buf[json_buf_len-1]='\0';
    return;
  }

  const struct satellite *satinfo = gps.getSatellites();
  for(uint8_t i = 0; i < MAX_SATELLITES && satinfo[i].id; i++) {
    const char *format = (i == 0) ? "[%u,%u,%u,%u]" : ",[%u,%u,%u,%u]";
    len += snprintf(json_buf + len, json_buf_len - len,
        format, satinfo[i].id, satinfo[i].elevation, satinfo[i].azimuth, satinfo[i].snr
        );
    if (len >= json_buf_len) {
      json_buf[json_buf_len-1] = '\0';
      return;
    }
  }
  snprintf(json_buf + len, json_buf_len - len, "]}}");
  json_buf[json_buf_len-1] = '\0';
}

void poll_system_stats() {
  uint32_t currentMillis = millis();
  if (PERIOD_EXPIRED(currentMillis,systemStatsTime,SYSTEM_STATS_PERIOD_MS) && (TEMPMON_TEMPSENSE0 & 0x4U)) {
    char msg[SYSTEM_STATS_STRLEN];
    struct udp_pcb *pcb;
    struct pbuf *pb;
    int len;

    compile_system_stats_json(msg, SYSTEM_STATS_STRLEN);
    len = strlen(msg);
    pb = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    pcb = udp_new();
    pbuf_take(pb, msg, len);
    udp_sendto_if(pcb, pb, &systemStatsAddr, /* 1025 */ 1028, netif_default);
    pbuf_free(pb);
    udp_remove(pcb);
    systemStatsTime = currentMillis;
  }
}

void multicast_nmea_string(const char *nmea) {
  if (!nmeaString) {
    int len = strlen(nmea)-1;
    nmeaString = (char*)malloc(len+1);
    if (nmeaString) strncpy(nmeaString, nmea, len);  // It will always have a CR or an LF at the end - see GPS.cpp
    nmeaString[len]=0;
  }
}

void send_pending_nmea_string() {
  if (nmeaString) {
    struct udp_pcb *pcb;
    struct pbuf *pb;
    int len = strlen(nmeaString);
    pb = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    pcb = udp_new();
    pbuf_take(pb, nmeaString, len);
    udp_sendto_if(pcb, pb, &nmeaAddr, 1026, netif_default);
    pbuf_free(pb);
    udp_remove(pcb);
    free(nmeaString);
    nmeaString = 0;
  }
}

#ifdef GPS_SECOND_SERIAL_PORT
void send_gps_raw() {
  if (rawGpsBufferLen) {
    struct udp_pcb *pcb;
    struct pbuf *pb;
    
    pb = pbuf_alloc(PBUF_TRANSPORT, rawGpsBufferLen, PBUF_RAM);
    pcb = udp_new();
    pbuf_take(pb, rawGpsBuffer, rawGpsBufferLen);
    udp_sendto_if(pcb, pb, &rawAddr, 1027, netif_default);
    pbuf_free(pb);
    udp_remove(pcb);
    rawGpsBufferLen = 0;
  }
  lastRawSendTime = millis();
}

void add_raw_gps_char(char c) {
  rawGpsBuffer[rawGpsBufferLen++] = c;
  if (rawGpsBufferLen == RAW_GPS_BUFFER_SIZE) {
    send_gps_raw();
  }
}

void poll_raw_multicast() {
  if (PERIOD_EXPIRED(millis(),lastRawSendTime,RAW_GPS_MAX_QUIET_MS)) send_gps_raw();
}
#endif
