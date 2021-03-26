#include <stdio.h>
#include <string.h>
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

#define SYSTEM_STATS_PERIOD_MS 300000
#define RAW_GPS_BUFFER_SIZE 1200
#define RAW_GPS_MAX_QUIET_MS 250

#define PERIOD_EXPIRED(nowMillis,startMillis,period) ((nowMillis - startMillis) >= period)

ip4_addr_t systemStatsAddr, nmeaAddr, rawAddr;
uint32_t systemStatsTime, lastRawSendTime;
char systemStatsTemplate[70];
char rawGpsBuffer[RAW_GPS_BUFFER_SIZE];
int rawGpsBufferLen = 0;

void setupMulticast() {
  uint8_t *mac = netif_default->hwaddr;
  IP4_ADDR(&systemStatsAddr, 239,0,0,1);
  IP4_ADDR(&nmeaAddr, 239,0,0,2);
  IP4_ADDR(&rawAddr, 239,0,0,3);
  systemStatsTime = millis() - SYSTEM_STATS_PERIOD_MS;
  strcpy(systemStatsTemplate, "{\"topic\":\"teensy-ntp-cpu-temperature\",\"payload\":%.1f}");
  lastRawSendTime = millis();
}

void pollSystemStats() {
  uint32_t currentMillis = millis();
  if (PERIOD_EXPIRED(currentMillis,systemStatsTime,SYSTEM_STATS_PERIOD_MS) && (TEMPMON_TEMPSENSE0 & 0x4U)) {
    char msg[70];
    struct udp_pcb *pcb;
    struct pbuf *pb;
    int len;

    snprintf(msg, 70, systemStatsTemplate, tempmonGetTemp());
    len = strlen(msg);
    pb = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    pcb = udp_new();
    pbuf_take(pb, msg, len);
    udp_sendto_if(pcb, pb, &systemStatsAddr, 1025, netif_default);
    pbuf_free(pb);
    udp_remove(pcb);
    systemStatsTime = currentMillis;
  }
}

void multicastNmeaString(const char *nmea) {
  struct udp_pcb *pcb;
  struct pbuf *pb;
  int len = strlen(nmea) - 1; // It will always have a CR or an LF at the end - see GPS.cpp
  pb = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
  pcb = udp_new();
  pbuf_take(pb, nmea, len);
  udp_sendto_if(pcb, pb, &nmeaAddr, 1026, netif_default);
  pbuf_free(pb);
  udp_remove(pcb);
}

void sendGpsRaw() {
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

void addRawGpsChar(char c) {
  rawGpsBuffer[rawGpsBufferLen++] = c;
  if (rawGpsBufferLen == RAW_GPS_BUFFER_SIZE) {
    sendGpsRaw();
  }
}

void pollRawMulticast() {
  if (PERIOD_EXPIRED(millis(),lastRawSendTime,RAW_GPS_MAX_QUIET_MS)) sendGpsRaw();
}
