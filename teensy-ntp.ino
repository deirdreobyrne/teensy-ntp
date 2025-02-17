#include "lwip_t41.h"
#include "lwip/inet.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "InputCapture.h"
#include "DateTime.h"
#include "GPS.h"
#include "NTPClock.h"
#include "ClockPID.h"
#include "NTPServer.h"
#include "NTPClients.h"
#include "platform-clock.h"
#include "WebServer.h"
#include "WebContent.h"
#include "MulticastServer.h"

// see the settings file for common settings
#include "settings.h"

#define WAIT_COUNT 3
#undef SERIAL_DEBUG

GPSDateTime gps(&GPS_SERIAL);
NTPClock localClock;
NTPClients clientList;
InputCapture pps;
elapsedMillis msec;
uint32_t compileTime;
uint8_t settime = 0;
uint8_t wait = WAIT_COUNT-1;
struct {
  int64_t offset;
  uint32_t pps;
  uint32_t gpstime;
} samples[WAIT_COUNT];
NTPServer server(&localClock);

char *nmea_tx_buffer = 0;
int nmea_tx_index = 0;
int nmea_tx_bytes_remaining = 0;
char *raw_tx_buffer = 0;
int raw_tx_index = 0;
int raw_tx_bytes_remaining = 0;

ip4_addr_t vlan1_ip, vlan1_mask, vlan1_gw, vlan3_ip, vlan3_mask, vlan3_gw;

#define UDP_LISTEN_ADDR (&vlan1_ip)

struct netif vlan3_netif;

static void netif_status_callback(struct netif *netif) {
  static char str1[IP4ADDR_STRLEN_MAX], str2[IP4ADDR_STRLEN_MAX], str3[IP4ADDR_STRLEN_MAX];
#ifdef SERIAL_DEBUG
  Serial.printf("netif %c%c status changed: ip %s, mask %s, gw %s\n", netif->name[0], netif->name[1], ip4addr_ntoa_r(netif_ip_addr4(netif), str1, IP4ADDR_STRLEN_MAX), ip4addr_ntoa_r(netif_ip_netmask4(netif), str2, IP4ADDR_STRLEN_MAX), ip4addr_ntoa_r(netif_ip_gw4(netif), str3, IP4ADDR_STRLEN_MAX));
#endif
}

static void link_status_callback(struct netif *netif) {
#ifdef SERIAL_DEBUG
  Serial.printf("enet netif %c%c link status: %s\n", netif->name[0], netif->name[1], netif_is_link_up(netif) ? "up" : "down");
#endif
}

void udp_nmea_callback(void * arg, struct udp_pcb * upcb, struct pbuf * p, const ip_addr_t * addr, u16_t port)
{
  if (p == NULL) return;
  if (nmea_tx_buffer == NULL) {
    nmea_tx_buffer = (char*)malloc(p->len);
    if (nmea_tx_buffer) {
      memcpy(nmea_tx_buffer, p->payload, p->len);
      nmea_tx_index = 0;
      nmea_tx_bytes_remaining = p->len;
    }
  }
  pbuf_free(p);
}

void udp_raw_callback(void * arg, struct udp_pcb * upcb, struct pbuf * p, const ip_addr_t * addr, u16_t port)
{
  if (p == NULL) return;
  if (raw_tx_buffer == NULL) {
    raw_tx_buffer = (char*)malloc(p->len);
    if (raw_tx_buffer) {
      memcpy(raw_tx_buffer, p->payload, p->len);
      raw_tx_index = 0;
      raw_tx_bytes_remaining = p->len;
    }
  }
  pbuf_free(p);
}

void nmea_raw_send_to_gps() {
  if (nmea_tx_bytes_remaining) {
    if (GPS_SERIAL.availableForWrite()) {
      GPS_SERIAL.write(nmea_tx_buffer[nmea_tx_index++]);
      nmea_tx_bytes_remaining--;
      if (!nmea_tx_bytes_remaining) {
        free(nmea_tx_buffer);
        nmea_tx_buffer = 0;
        nmea_tx_index = 0;
      }
    }
  }
#ifdef GPS_SECOND_SERIAL_PORT
  if (raw_tx_bytes_remaining) {
    if (GPS_SECOND_SERIAL_PORT.availableForWrite()) {
      GPS_SECOND_SERIAL_PORT.write(raw_tx_buffer[raw_tx_index++]);
      raw_tx_bytes_remaining--;
      if (!raw_tx_bytes_remaining) {
        free(raw_tx_buffer);
        raw_tx_buffer = 0;
        raw_tx_index = 0;
      }
    }
  }
#endif
}

void udp_nmea_raw_srv() {
  struct udp_pcb *pcb;

#ifdef SERIAL_DEBUG
  Serial.println("udp nmea srv on port 2048");
#endif
  pcb = udp_new();
  udp_bind(pcb, UDP_LISTEN_ADDR, 2048);    // local port
  udp_recv(pcb, udp_nmea_callback, NULL);  // do once?
#ifdef SERIAL_DEBUG
  Serial.println("udp raw srv on port 2049");
#endif
  udp_bind(pcb, UDP_LISTEN_ADDR, 2049);    // local port
  udp_recv(pcb, udp_raw_callback, NULL);  // do once?
  // fall into loop  ether_poll
}

void setup() {
#ifdef SERIAL_DEBUG
  Serial.begin(115200);
#endif

  DateTime compile = DateTime(__DATE__, __TIME__);

  GPS_SERIAL.begin(GPS_BAUD);
#ifdef GPS_SECOND_SERIAL_PORT
  GPS_SECOND_SERIAL_PORT.begin(GPS_BAUD);
#endif

#ifdef SERIAL_DEBUG
  Serial.println("Ethernet 1588 NTP Server");
  Serial.println("------------------------\n");
#endif

  IP4_ADDR(&vlan1_ip,172,17,1,3);
  IP4_ADDR(&vlan1_mask,255,255,255,0);
  IP4_ADDR(&vlan1_gw,172,17,1,1);
  IP4_ADDR(&vlan3_ip,172,17,2,3);
  IP4_ADDR(&vlan3_mask,255,255,255,0);
  IP4_ADDR(&vlan3_gw,172,17,2,1);

  enet_init(NULL, NULL, NULL);

  netif_set_status_callback(netif_default, netif_status_callback);
  netif_set_link_callback(netif_default, link_status_callback);
  netif_set_vlan_id(netif_default, 1);
  netif_set_hostname(netif_default, DHCP_HOSTNAME);
  netif_set_ipaddr(netif_default, &vlan1_ip);
  netif_set_netmask(netif_default, &vlan1_mask);
  netif_set_gw(netif_default, &vlan1_gw);
  netif_set_up(netif_default);

  netif_add_vlan(&vlan3_netif, &vlan3_ip, &vlan3_mask, &vlan3_gw, 3, 0, t41_extra_netif_init, 0);
  netif_set_status_callback(&vlan3_netif, netif_status_callback);
  netif_set_link_callback(&vlan3_netif, link_status_callback);
  netif_set_hostname(&vlan3_netif, DHCP_HOSTNAME "-vlan3");

//  dhcp_start(netif_default);
//  dhcp_start(&vlan3_netif);

#ifdef SERIAL_DEBUG
  Serial.println("waiting for link");
#endif
  while (!netif_is_link_up(netif_default)) {
    enet_proc_input(); // await on link up
    enet_poll();
    delay(1);
  }
  netif_set_up(&vlan3_netif);
  netif_set_link_up(&vlan3_netif);

  pps.begin();
  server.setup();

  compileTime = compile.ntptime();
  // this needs to happen after enet_init, so the 1588 clock is running
  localClock.setTime(COUNTERFUNC(), compileTime);
  // allow for compile timezone to be 12 hours ahead
  compileTime -= 12*60*60;

  webserver.begin();
  webcontent.begin();

  while(GPS_SERIAL.available()) { // throw away all the text received while starting up
    GPS_SERIAL.read();
  }
#ifdef GPS_SECOND_SERIAL_PORT
  while(GPS_SECOND_SERIAL_PORT.available()) { // throw away all the text received while starting up
    GPS_SECOND_SERIAL_PORT.read();
  }
#endif
  msec = 0;
  setup_multicast();
  udp_nmea_raw_srv();
}

static uint8_t median(int64_t one, int64_t two, int64_t three) {
  if(one > two) {
    if(one > three) {
      if(two > three) {
        // 1 2 3
        return 2-1;
      } else {
        // 1 3 2
        return 3-1;
      }
    } else {
      // 3 1 2
      return 1-1;
    }
  } else {
    if(two > three) {
      if(one > three) {
        // 2 1 3
        return 1-1;
      } else {
        // 2 3 1
        return 3-1;
      }
    } else {
      // 3 2 1
      return 2-1;
    }
  }
}

static uint32_t ntp64_to_32(int64_t offset) {
  if(offset < 0)
    offset *= -1;
  // take 16bits off the bottom and top
  offset = offset >> 16;
  return offset & 0xffffffff;
}

void updateTime(uint32_t gpstime) {
  if(gps.ppsMillis() == 0) {
    return;
  }

  uint32_t ppsToGPS = gps.capturedAt() - gps.ppsMillis();
  webcontent.setPPSData(ppsToGPS, gps.ppsMillis(), gpstime);
  if(ppsToGPS > 950) { // allow 950ms between PPS and GPS message
#ifdef SERIAL_DEBUG
    Serial.print("LAG ");
    Serial.print(ppsToGPS);
    Serial.print(" ");
    Serial.print(gps.ppsMillis());
    Serial.print(" ");
    Serial.println(gpstime);
#endif
    return;
  }

  uint32_t lastPPS = gps.ppsCounter();
  if(settime) {
    int64_t offset = localClock.getOffset(lastPPS, gpstime, 0);
    samples[wait].offset = offset;
    samples[wait].pps = lastPPS;
    samples[wait].gpstime = gpstime;
    if(ClockPID.full() && wait) {
      wait--;
    } else {
      uint8_t median_index = wait;
      if(wait == 0) {
        median_index = median(samples[0].offset, samples[1].offset, samples[2].offset);
      }
      ClockPID.add_sample(samples[median_index].pps, samples[median_index].gpstime, samples[median_index].offset);
      localClock.setRefTime(samples[median_index].gpstime);
      localClock.setPpb(ClockPID.out() * 1000000000.0);
      wait = WAIT_COUNT-1; // (2+1)*16=48s, 80MHz wraps at 53s

      // TODO: this should grow when out of sync
      server.setDispersion(ntp64_to_32(samples[median_index].offset));
      server.setReftime(samples[median_index].gpstime);

      double offsetHuman = samples[median_index].offset / (double)4294967296.0;
      webcontent.setLocalClock(samples[median_index].pps, offsetHuman, ClockPID.d(), ClockPID.d_chi(), localClock.getPpb(), gpstime);
#ifdef SERIAL_DEBUG
      Serial.print(samples[median_index].pps);
      Serial.print(" ");
      Serial.print(offsetHuman, 9);
      Serial.print(" ");
      Serial.print(ClockPID.d(), 9);
      Serial.print(" ");
      Serial.print(ClockPID.d_chi(), 9);
      Serial.print(" ");
      Serial.print(localClock.getPpb());
      Serial.print(" ");
      Serial.println(samples[median_index].gpstime);
#endif
    }
  } else {
    localClock.setTime(lastPPS, gpstime);
    ClockPID.add_sample(lastPPS, gpstime, 0);
    settime = 1;
#ifdef SERIAL_DEBUG
    Serial.print("S "); // clock set message
    Serial.print(lastPPS);
    Serial.print(" ");
    Serial.println(gpstime);
#endif
  }
}

static void slower_poll() {
  if(msec >= 1000) {
    uint32_t s, s_fb;
    // update the local clock's cycle count
    localClock.getTime(COUNTERFUNC(),&s,&s_fb);

    // check link state, update dhcp, etc
    enet_poll();

    // remove old NTP clients
    clientList.expireClients();

    msec = 0;
  }
}

static void gps_serial_poll() {
  if(GPS_SERIAL.available()) {
    if(gps.decode()) {
      uint32_t gpstime = gps.GPSnow().ntptime();
      if(gpstime < compileTime) {
#ifdef SERIAL_DEBUG
        Serial.print("B "); // gps clock bad message (for example, on startup before GPS almanac)
        Serial.println(gpstime);
#endif
      } else {
        updateTime(gpstime);
      }
    }
  }
}

// useful when using teensy_loader_cli
static void bootloader_poll() {
#ifdef SERIAL_DEBUG
  if(Serial.available()) {
    char r = Serial.read();
    if(r == 'r') {
      Serial.println("rebooting to bootloader");
      delay(10);
      asm("bkpt #251"); // run bootloader
    }
  }
#endif
}

#ifdef GPS_SECOND_SERIAL_PORT
void poll_raw_input() {
  if (GPS_SECOND_SERIAL_PORT.available()) {
    add_raw_gps_char(GPS_SECOND_SERIAL_PORT.read());
  }
}
#endif

void loop() {
  enet_proc_input();

  slower_poll();

  enet_proc_input();

  gps_serial_poll();

#ifdef SERIAL_DEBUG
  enet_proc_input();

  bootloader_poll(); // <-- Needs the USB serial port
#endif

  enet_proc_input();

  poll_system_stats();

#ifdef GPS_SECOND_SERIAL_PORT
  enet_proc_input();

  poll_raw_multicast();

  enet_proc_input();

  poll_raw_input();
#endif

  enet_proc_input();

  send_pending_nmea_string();

  enet_proc_input();

  nmea_raw_send_to_gps();
}
