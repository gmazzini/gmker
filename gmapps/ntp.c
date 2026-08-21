// Gianluca Mazzini @2026- Version 1.0
#include "../gmprog.h"

#define NTP_PORT 123U
#define NTP_PACKET 48U
#define NTP_UNIX_OFFSET 2208988800ULL
#define NTP_ERA 4294967296ULL

static int parse_ip(const char *s,uint64_t len,uint8_t ip[4]) {
  uint64_t i;
  uint64_t part;
  uint64_t value;
  uint64_t digits;

  i=0ULL;
  part=0ULL;
  for (;part<4ULL;part++) {
    value=0ULL;
    digits=0ULL;
    for (;i<len && s[i]>='0' && s[i]<='9';i++) {
      value=value*10ULL+(uint64_t)(s[i]-'0');
      if (value>255ULL) return 0;
      digits++;
    }
    if (!digits) return 0;
    ip[part]=(uint8_t)value;
    if (part<3ULL) {
      if (i>=len || s[i]!='.') return 0;
      i++;
    }
  }
  return i==len;
}

static int leap(uint64_t year) {
  return !(year%4ULL) && ((year%100ULL) || !(year%400ULL));
}

static void print2(uint64_t value) {
  char text[3];

  text[0]=(char)('0'+(value/10ULL)%10ULL);
  text[1]=(char)('0'+value%10ULL);
  text[2]=0;
  gm_write(text);
}

static void print_utc(uint64_t timestamp) {
  static const uint8_t month_days[12]={31,28,31,30,31,30,31,31,30,31,30,31};
  uint64_t days;
  uint64_t remain;
  uint64_t year;
  uint64_t month;
  uint64_t count;
  uint64_t hour;
  uint64_t minute;
  uint64_t second;

  days=timestamp/86400ULL;
  remain=timestamp%86400ULL;
  year=1970ULL;
  for (;;) {
    count=leap(year)?366ULL:365ULL;
    if (days<count) break;
    days-=count;
    year++;
  }
  month=0ULL;
  for (;month<12ULL;month++) {
    count=month_days[month];
    if (month==1ULL && leap(year)) count++;
    if (days<count) break;
    days-=count;
  }
  hour=remain/3600ULL;
  remain%=3600ULL;
  minute=remain/60ULL;
  second=remain%60ULL;
  gm_print_u64(year);
  gm_write("-");
  print2(month+1ULL);
  gm_write("-");
  print2(days+1ULL);
  gm_write(" ");
  print2(hour);
  gm_write(":");
  print2(minute);
  gm_write(":");
  print2(second);
}

int gm_main(const char *args,uint64_t arg_len) {
  uint8_t ip[4];
  uint8_t request[NTP_PACKET];
  uint8_t reply[NTP_PACKET];
  uint64_t ntp;
  uint64_t unix_time;
  uint64_t i;
  uint16_t got;
  int released;

  if (!parse_ip(args,arg_len,ip)) {
    gm_write("usage: run ntp IPv4\n");
    return 1;
  }
  for (i=0ULL;i<NTP_PACKET;i++) request[i]=0;
  request[0]=0x23U;
  if (!gm_resource_acquire(GM_RESOURCE_UDP)) {
    gm_write("ntp: udp unavailable\n");
    return 2;
  }
  got=gm_udp_exchange(ip,NTP_PORT,request,NTP_PACKET,reply,NTP_PACKET);
  released=gm_resource_release(GM_RESOURCE_UDP);
  if (!released) return 3;
  if (got<NTP_PACKET) {
    gm_write("ntp: no reply\n");
    return 4;
  }
  if ((reply[0]&7U)!=4U || (reply[0]>>6)==3U || !reply[1] || reply[1]>15U) {
    gm_write("ntp: invalid reply\n");
    return 5;
  }
  ntp=((uint64_t)reply[40]<<24)|((uint64_t)reply[41]<<16)|((uint64_t)reply[42]<<8)|reply[43];
  if (ntp<NTP_UNIX_OFFSET) ntp+=NTP_ERA;
  unix_time=ntp-NTP_UNIX_OFFSET;
  gm_write("NTP unix=");
  gm_print_u64(unix_time);
  gm_write(" UTC=");
  print_utc(unix_time);
  gm_write("\n");
  return 0;
}
