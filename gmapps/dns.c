// Gianluca Mazzini @2026- Version 1.0
#include "../gmprog.h"

#define DNS_PORT 53U
#define DNS_HEADER 12U
#define DNS_TYPE_A 1U
#define DNS_TYPE_CNAME 5U
#define DNS_CLASS_IN 1U
#define DNS_NAME_MAX 255U
#define DNS_JUMPS_MAX 32U

static uint16_t dns_u16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);
}

static uint32_t dns_u32(const uint8_t *p) {
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static void dns_put16(uint8_t *p,uint16_t value) {
  p[0]=(uint8_t)(value>>8);
  p[1]=(uint8_t)value;
}

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

static int dns_args(const char *args,uint64_t arg_len,uint8_t ip[4],const char **name,uint64_t *name_len) {
  uint64_t split;
  uint64_t start;

  split=0ULL;
  for (;split<arg_len && args[split]!=' ';split++);
  if (!split || split>=arg_len || !parse_ip(args,split,ip)) return 0;
  start=split;
  for (;start<arg_len && args[start]==' ';start++);
  if (start>=arg_len) return 0;
  *name=args+start;
  *name_len=arg_len-start;
  return 1;
}

static int dns_build_query(const char *name,uint64_t name_len,uint16_t id,uint8_t *packet,uint16_t *packet_len) {
  uint64_t start;
  uint64_t i;
  uint64_t label_len;
  uint16_t out;

  if (!name || !name_len || name_len>DNS_NAME_MAX || !packet || !packet_len) return 0;
  for (;name_len && name[name_len-1ULL]=='.';name_len--);
  if (!name_len) return 0;
  for (i=0ULL;i<DNS_HEADER;i++) packet[i]=0;
  dns_put16(packet,id);
  dns_put16(packet+4,1U);
  out=DNS_HEADER;
  start=0ULL;
  for (i=0ULL;i<=name_len;i++) {
    if (i<name_len && name[i]!='.') continue;
    label_len=i-start;
    if (!label_len || label_len>63ULL || (uint32_t)out+1U+label_len+5U>GM_UDP_MAX) return 0;
    packet[out++]=(uint8_t)label_len;
    for (;start<i;start++) packet[out++]=(uint8_t)name[start];
    start=i+1ULL;
  }
  packet[out++]=0;
  dns_put16(packet+out,DNS_TYPE_A);
  out=(uint16_t)(out+2U);
  dns_put16(packet+out,DNS_CLASS_IN);
  out=(uint16_t)(out+2U);
  *packet_len=out;
  return 1;
}

static int dns_name(const uint8_t *packet,uint16_t packet_len,uint16_t offset,char *name,uint16_t name_cap,uint16_t *next) {
  uint16_t pos;
  uint16_t target;
  uint16_t out;
  uint16_t after;
  uint16_t jumps;
  uint8_t label;
  uint8_t i;
  int jumped;

  if (!packet || offset>=packet_len || !name || name_cap<2U) return 0;
  pos=offset;
  out=0;
  after=0;
  jumps=0;
  jumped=0;
  for (;;) {
    if (pos>=packet_len) return 0;
    label=packet[pos];
    if ((label&0xc0U)==0xc0U) {
      if ((uint32_t)pos+1U>=packet_len || jumps++>=DNS_JUMPS_MAX) return 0;
      target=(uint16_t)(((uint16_t)(label&0x3fU)<<8)|packet[pos+1U]);
      if (target>=packet_len) return 0;
      if (!jumped) after=(uint16_t)(pos+2U);
      pos=target;
      jumped=1;
      continue;
    }
    if (label&0xc0U) return 0;
    pos++;
    if (!label) {
      if (!jumped) after=pos;
      if (!out) {
        if (name_cap<2U) return 0;
        name[out++]='.';
      }
      name[out]=0;
      if (next) *next=after;
      return 1;
    }
    if (label>63U || (uint32_t)pos+label>packet_len) return 0;
    if (out) {
      if ((uint32_t)out+1U>=name_cap) return 0;
      name[out++]='.';
    }
    if ((uint32_t)out+label>=name_cap) return 0;
    for (i=0;i<label;i++) name[out++]=(char)packet[pos+i];
    pos=(uint16_t)(pos+label);
  }
}

static void dns_print_ip(const uint8_t ip[4]) {
  uint64_t i;

  for (i=0ULL;i<4ULL;i++) {
    gm_print_u64(ip[i]);
    if (i<3ULL) gm_write(".");
  }
}

static int dns_parse_response(const uint8_t *packet,uint16_t packet_len,uint16_t id) {
  char owner[DNS_NAME_MAX+1U];
  char target[DNS_NAME_MAX+1U];
  uint16_t offset;
  uint16_t next;
  uint16_t type;
  uint16_t class_id;
  uint16_t rdlen;
  uint16_t answers;
  uint16_t i;
  uint16_t flags;
  uint32_t ttl;
  int found;

  if (!packet || packet_len<DNS_HEADER || dns_u16(packet)!=id) return 0;
  flags=dns_u16(packet+2U);
  if (!(flags&0x8000U) || (flags&0x7800U) || (flags&0x0200U) || (flags&0x000fU)) return 0;
  if (dns_u16(packet+4U)!=1U) return 0;
  answers=dns_u16(packet+6U);
  offset=DNS_HEADER;
  if (!dns_name(packet,packet_len,offset,owner,sizeof(owner),&offset) || (uint32_t)offset+4U>packet_len) return 0;
  offset=(uint16_t)(offset+4U);
  found=0;
  for (i=0;i<answers;i++) {
    if (!dns_name(packet,packet_len,offset,owner,sizeof(owner),&next) || (uint32_t)next+10U>packet_len) return 0;
    type=dns_u16(packet+next);
    class_id=dns_u16(packet+next+2U);
    ttl=dns_u32(packet+next+4U);
    rdlen=dns_u16(packet+next+8U);
    offset=(uint16_t)(next+10U);
    if ((uint32_t)offset+rdlen>packet_len) return 0;
    if (class_id==DNS_CLASS_IN && type==DNS_TYPE_A && rdlen==4U) {
      gm_write("A ");
      gm_write(owner);
      gm_write(" ");
      dns_print_ip(packet+offset);
      gm_write(" ttl=");
      gm_print_u64(ttl);
      gm_write("\n");
      found=1;
    } else if (class_id==DNS_CLASS_IN && type==DNS_TYPE_CNAME) {
      if (!dns_name(packet,packet_len,offset,target,sizeof(target),0)) return 0;
      gm_write("CNAME ");
      gm_write(owner);
      gm_write(" ");
      gm_write(target);
      gm_write(" ttl=");
      gm_print_u64(ttl);
      gm_write("\n");
      found=1;
    }
    offset=(uint16_t)(offset+rdlen);
  }
  return found;
}

int gm_main(const char *args,uint64_t arg_len) {
  uint8_t ip[4];
  uint8_t query[GM_UDP_MAX];
  uint8_t reply[GM_UDP_MAX];
  const char *name;
  uint64_t name_len;
  uint16_t query_len;
  uint16_t reply_len;
  uint16_t id;
  int released;

  if (!dns_args(args,arg_len,ip,&name,&name_len)) {
    gm_write("usage: run dns DNS_SERVER_IP name\n");
    return 1;
  }
  id=(uint16_t)(gm_ticks()^0x474dU);
  if (!dns_build_query(name,name_len,id,query,&query_len)) {
    gm_write("dns: invalid name\n");
    return 2;
  }
  if (!gm_resource_acquire(GM_RESOURCE_UDP)) {
    gm_write("dns: udp unavailable\n");
    return 3;
  }
  reply_len=gm_udp_exchange(ip,DNS_PORT,query,query_len,reply,sizeof(reply));
  released=gm_resource_release(GM_RESOURCE_UDP);
  if (!released) return 4;
  if (!reply_len) {
    gm_write("dns: no reply\n");
    return 5;
  }
  if (!dns_parse_response(reply,reply_len,id)) {
    gm_write("dns: no A/CNAME answer\n");
    return 6;
  }
  return 0;
}
