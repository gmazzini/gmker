// Gianluca Mazzini @2026- Version 2.02
#include "gmker.h"

#define GM_ETH_ARP 0x0806
#define GM_ETH_IPV4 0x0800
#define GM_ARP_MAX 8
#define GM_PROTO_ICMP 1
#define GM_PROTO_TCP 6

struct gm_arp_packet {
  uint16_t htype;
  uint16_t ptype;
  uint8_t hlen;
  uint8_t plen;
  uint16_t op;
  uint8_t sha[6];
  uint8_t spa[4];
  uint8_t tha[6];
  uint8_t tpa[4];
} __attribute__((packed));

struct gm_ipv4_header {
  uint8_t version_ihl;
  uint8_t tos;
  uint16_t total;
  uint16_t id;
  uint16_t fragment;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t checksum;
  uint8_t src[4];
  uint8_t dst[4];
} __attribute__((packed));

struct gm_arp_entry {
  uint8_t used;
  uint8_t ip[4];
  uint8_t mac[6];
};

static uint8_t gm_mac[6];
static struct gm_arp_entry gm_arp[GM_ARP_MAX];
static uint16_t gm_ip_id;
static uint16_t gm_ping_seq;
static volatile uint16_t gm_ping_reply;
static uint64_t gm_rx_packets;
static uint64_t gm_tx_packets;
static int gm_net_online;

static int gm_eth_send(const uint8_t dst[6],uint16_t type,const uint8_t *data,uint16_t len) {
  uint8_t frame[1518];
  uint16_t frame_len;

  if ((uint32_t)len+14U>sizeof(frame)) return 0;
  gm_memcpy(frame,dst,6);
  gm_memcpy(frame+6,gm_mac,6);
  frame[12]=(uint8_t)(type>>8);
  frame[13]=(uint8_t)type;
  gm_memcpy(frame+14,data,len);
  frame_len=(uint16_t)(len+14U);
  if (frame_len<60U) {
    gm_memset(frame+frame_len,0,60U-frame_len);
    frame_len=60U;
  }
  if (!gm_virtio_net_send(frame,frame_len)) return 0;
  gm_tx_packets++;
  return 1;
}

static void gm_arp_put(const uint8_t ip[4],const uint8_t mac[6]) {
  int free_slot;
  int i;

  free_slot=-1;
  for (i=0;i<GM_ARP_MAX;i++) {
    if (gm_arp[i].used && gm_ip_eq(gm_arp[i].ip,ip)) {
      gm_memcpy(gm_arp[i].mac,mac,6);
      return;
    }
    if (!gm_arp[i].used && free_slot<0) free_slot=i;
  }
  if (free_slot<0) free_slot=0;
  gm_arp[free_slot].used=1;
  gm_copy_ip(gm_arp[free_slot].ip,ip);
  gm_memcpy(gm_arp[free_slot].mac,mac,6);
}

static int gm_arp_lookup(const uint8_t ip[4],uint8_t mac[6]) {
  int i;

  for (i=0;i<GM_ARP_MAX;i++) {
    if (gm_arp[i].used && gm_ip_eq(gm_arp[i].ip,ip)) {
      gm_memcpy(mac,gm_arp[i].mac,6);
      return 1;
    }
  }
  return 0;
}

static int gm_same_subnet(const uint8_t a[4],const uint8_t b[4]) {
  int i;

  for (i=0;i<4;i++) if ((a[i]&gm_config.mask[i])!=(b[i]&gm_config.mask[i])) return 0;
  return 1;
}

static void gm_next_hop(const uint8_t dst[4],uint8_t hop[4]) {
  if (gm_same_subnet(gm_config.ip,dst)) gm_copy_ip(hop,dst);
  else gm_copy_ip(hop,gm_config.gateway);
}

static void gm_arp_request(const uint8_t ip[4]) {
  struct gm_arp_packet packet;
  uint8_t broadcast[6];

  gm_memset(&packet,0,sizeof(packet));
  gm_memset(broadcast,0xff,6);
  packet.htype=gm_swap16(1);
  packet.ptype=gm_swap16(GM_ETH_IPV4);
  packet.hlen=6;
  packet.plen=4;
  packet.op=gm_swap16(1);
  gm_memcpy(packet.sha,gm_mac,6);
  gm_copy_ip(packet.spa,gm_config.ip);
  gm_copy_ip(packet.tpa,ip);
  (void)gm_eth_send(broadcast,GM_ETH_ARP,(const uint8_t *)&packet,sizeof(packet));
}

static void gm_arp_reply(const struct gm_arp_packet *request) {
  struct gm_arp_packet packet;

  gm_memset(&packet,0,sizeof(packet));
  packet.htype=gm_swap16(1);
  packet.ptype=gm_swap16(GM_ETH_IPV4);
  packet.hlen=6;
  packet.plen=4;
  packet.op=gm_swap16(2);
  gm_memcpy(packet.sha,gm_mac,6);
  gm_copy_ip(packet.spa,gm_config.ip);
  gm_memcpy(packet.tha,request->sha,6);
  gm_copy_ip(packet.tpa,request->spa);
  (void)gm_eth_send(request->sha,GM_ETH_ARP,(const uint8_t *)&packet,sizeof(packet));
}

static void gm_arp_input(const uint8_t *data,uint16_t len) {
  const struct gm_arp_packet *packet;
  uint16_t op;

  if (len<sizeof(struct gm_arp_packet)) return;
  packet=(const struct gm_arp_packet *)data;
  if (gm_swap16(packet->htype)!=1 || gm_swap16(packet->ptype)!=GM_ETH_IPV4 ||
      packet->hlen!=6 || packet->plen!=4) return;
  gm_arp_put(packet->spa,packet->sha);
  op=gm_swap16(packet->op);
  if (op==1 && gm_ip_eq(packet->tpa,gm_config.ip)) gm_arp_reply(packet);
}

uint16_t gm_checksum(const void *data,uint16_t len) {
  const uint8_t *p;
  uint32_t sum;

  p=(const uint8_t *)data;
  sum=0;
  while (len>1U) {
    sum+=((uint16_t)p[0]<<8)|p[1];
    p+=2;
    len-=2;
  }
  if (len) sum+=(uint16_t)p[0]<<8;
  while (sum>>16) sum=(sum&0xffffU)+(sum>>16);
  return (uint16_t)~sum;
}

int gm_net_prepare(const uint8_t dst[4],uint64_t timeout) {
  uint8_t hop[4];
  uint8_t mac[6];
  uint64_t start;
  uint64_t last;

  if (!gm_net_online) return 0;
  gm_next_hop(dst,hop);
  if (gm_arp_lookup(hop,mac)) return 1;
  start=gm_ticks();
  last=start-timeout;
  while (gm_ticks()-start<timeout) {
    if (gm_ticks()-last>=10U) {
      gm_arp_request(hop);
      last=gm_ticks();
    }
    gm_pump();
    if (gm_arp_lookup(hop,mac)) return 1;
  }
  return 0;
}

int gm_ipv4_send(const uint8_t dst[4],uint8_t protocol,const uint8_t *data,uint16_t len) {
  uint8_t packet[GM_NET_MTU];
  uint8_t hop[4];
  uint8_t mac[6];
  struct gm_ipv4_header *hdr;

  if ((uint32_t)len+sizeof(struct gm_ipv4_header)>sizeof(packet)) return 0;
  gm_next_hop(dst,hop);
  if (!gm_arp_lookup(hop,mac)) return 0;
  hdr=(struct gm_ipv4_header *)packet;
  hdr->version_ihl=0x45;
  hdr->tos=0;
  hdr->total=gm_swap16((uint16_t)(sizeof(*hdr)+len));
  hdr->id=gm_swap16(++gm_ip_id);
  hdr->fragment=gm_swap16(0x4000);
  hdr->ttl=64;
  hdr->protocol=protocol;
  hdr->checksum=0;
  gm_copy_ip(hdr->src,gm_config.ip);
  gm_copy_ip(hdr->dst,dst);
  hdr->checksum=gm_swap16(gm_checksum(hdr,sizeof(*hdr)));
  gm_memcpy(packet+sizeof(*hdr),data,len);
  return gm_eth_send(mac,GM_ETH_IPV4,packet,(uint16_t)(sizeof(*hdr)+len));
}

static void gm_icmp_input(const uint8_t src[4],const uint8_t *data,uint16_t len) {
  uint8_t reply[GM_NET_MTU];
  uint16_t sum;

  if (len<8U || gm_checksum(data,len)!=0) return;
  if (data[0]==0 && data[1]==0 && gm_swap16(*(const uint16_t *)(data+6))==gm_ping_seq) {
    gm_ping_reply=gm_ping_seq;
    return;
  }
  if (data[0]!=8 || data[1]!=0 || len>sizeof(reply)) return;
  gm_memcpy(reply,data,len);
  reply[0]=0;
  reply[2]=0;
  reply[3]=0;
  sum=gm_checksum(reply,len);
  reply[2]=(uint8_t)(sum>>8);
  reply[3]=(uint8_t)sum;
  (void)gm_ipv4_send(src,GM_PROTO_ICMP,reply,len);
}

void gm_ipv4_input(const uint8_t src[4],uint8_t protocol,const uint8_t *data,uint16_t len) {
  if (protocol==GM_PROTO_ICMP) gm_icmp_input(src,data,len);
  else if (protocol==GM_PROTO_TCP) gm_tcp_input(src,data,len);
}

static void gm_ipv4_packet(const uint8_t *data,uint16_t len) {
  const struct gm_ipv4_header *hdr;
  uint16_t header_len;
  uint16_t total;
  uint16_t fragment;
  static const uint8_t broadcast[4]={255,255,255,255};

  if (len<sizeof(struct gm_ipv4_header)) return;
  hdr=(const struct gm_ipv4_header *)data;
  if ((hdr->version_ihl>>4)!=4 || (hdr->version_ihl&0x0fU)<5U) return;
  header_len=(uint16_t)(hdr->version_ihl&0x0fU)*4U;
  total=gm_swap16(hdr->total);
  fragment=gm_swap16(hdr->fragment);
  if (header_len>len || total<header_len || total>len) return;
  if (fragment&0xbfffU) return;
  if (gm_checksum(hdr,header_len)!=0) return;
  if (!gm_ip_eq(hdr->dst,gm_config.ip) && !gm_ip_eq(hdr->dst,broadcast)) return;
  gm_ipv4_input(hdr->src,hdr->protocol,data+header_len,(uint16_t)(total-header_len));
}

static void gm_eth_input(const uint8_t *frame,uint16_t len) {
  uint16_t type;

  if (len<14U) return;
  type=((uint16_t)frame[12]<<8)|frame[13];
  if (type==GM_ETH_ARP) gm_arp_input(frame+14,(uint16_t)(len-14U));
  else if (type==GM_ETH_IPV4) gm_ipv4_packet(frame+14,(uint16_t)(len-14U));
}

void gm_net_poll(void) {
  uint8_t frame[1518];
  uint16_t len;
  int rc;

  if (!gm_net_online) return;
  for (;;) {
    rc=gm_virtio_net_poll(frame,sizeof(frame),&len);
    if (rc<0) {
      gm_net_online=0;
      return;
    }
    if (!rc) return;
    gm_rx_packets++;
    if (len>=14U) gm_eth_input(frame,len);
  }
}

int gm_ping(const uint8_t dst[4],uint64_t timeout) {
  uint8_t packet[24];
  uint16_t sum;
  uint64_t start;

  if (gm_ip_eq(dst,gm_config.ip)) return 1;
  if (!gm_net_prepare(dst,timeout)) return 0;
  gm_memset(packet,0,sizeof(packet));
  packet[0]=8;
  packet[4]=0x47;
  packet[5]=0x4d;
  gm_ping_seq++;
  packet[6]=(uint8_t)(gm_ping_seq>>8);
  packet[7]=(uint8_t)gm_ping_seq;
  gm_memcpy(packet+8,"gmker 3.0 ping",14);
  sum=gm_checksum(packet,sizeof(packet));
  packet[2]=(uint8_t)(sum>>8);
  packet[3]=(uint8_t)sum;
  gm_ping_reply=0;
  if (!gm_ipv4_send(dst,GM_PROTO_ICMP,packet,sizeof(packet))) return 0;
  start=gm_ticks();
  while (gm_ticks()-start<timeout) {
    gm_pump();
    if (gm_ping_reply==gm_ping_seq) return 1;
  }
  return 0;
}

void gm_net_init(void) {
  gm_memset(gm_arp,0,sizeof(gm_arp));
  gm_ip_id=0;
  gm_ping_seq=0;
  gm_ping_reply=0;
  gm_rx_packets=0;
  gm_tx_packets=0;
  gm_net_online=0;
  if (gm_virtio_net_init(gm_mac)) {
    gm_net_online=1;
    gm_write("net virtio ");
    gm_print_ip(gm_config.ip);
    gm_write("\n");
  } else {
    gm_write("net offline\n");
  }
}


void gm_net_status(void) {
  gm_write("net state=");
  gm_write(gm_net_online?"up":"offline");
  gm_write(" ip=");
  gm_print_ip(gm_config.ip);
  gm_write(" mask=");
  gm_print_ip(gm_config.mask);
  gm_write(" gw=");
  gm_print_ip(gm_config.gateway);
  gm_write(" rx=");
  gm_print_u64(gm_rx_packets);
  gm_write(" tx=");
  gm_print_u64(gm_tx_packets);
  gm_write("\n");
}

void gm_arp_status(void) {
  int i;

  for (i=0;i<GM_ARP_MAX;i++) {
    if (!gm_arp[i].used) continue;
    gm_print_ip(gm_arp[i].ip);
    gm_write(" -> ");
    gm_print_hex(((uint64_t)gm_arp[i].mac[0]<<40)|((uint64_t)gm_arp[i].mac[1]<<32)|
                 ((uint64_t)gm_arp[i].mac[2]<<24)|((uint64_t)gm_arp[i].mac[3]<<16)|
                 ((uint64_t)gm_arp[i].mac[4]<<8)|gm_arp[i].mac[5]);
    gm_write("\n");
  }
}
