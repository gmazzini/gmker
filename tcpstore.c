// Gianluca Mazzini @2026- Version 2.03
#include "gmker.h"

#define GM_PROTO_TCP 6
#define GM_TCP_CLOSED 0
#define GM_TCP_SYN 1
#define GM_TCP_ESTABLISHED 2
#define GM_TCP_FIN 3
#define GM_TCP_RESET 4
#define GM_TCP_FLAG_FIN 0x01
#define GM_TCP_FLAG_SYN 0x02
#define GM_TCP_FLAG_RST 0x04
#define GM_TCP_FLAG_PSH 0x08
#define GM_TCP_FLAG_ACK 0x10

struct gm_tcp_header {
  uint16_t src;
  uint16_t dst;
  uint32_t seq;
  uint32_t ack;
  uint8_t offset;
  uint8_t flags;
  uint16_t window;
  uint16_t checksum;
  uint16_t urgent;
} __attribute__((packed));

static uint8_t gm_tcp_dst[4];
static uint16_t gm_tcp_src_port;
static uint16_t gm_tcp_dst_port;
static uint32_t gm_tcp_seq;
static uint32_t gm_tcp_ack;
static uint8_t gm_tcp_state;
static uint8_t gm_tcp_rx[GM_TCP_RX_SIZE];
static uint16_t gm_tcp_rx_head;
static uint16_t gm_tcp_rx_len;
static uint8_t gm_tcp_unacked[GM_TCP_TX_MAX];
static uint16_t gm_tcp_unacked_len;
static uint32_t gm_tcp_unacked_seq;
static uint64_t gm_tcp_unacked_tick;
static uint8_t gm_tcp_unacked_retry;
static uint64_t gm_tcp_tx_count;
static uint64_t gm_tcp_rx_count;
static uint64_t gm_tcp_drop_count;
static uint16_t gm_next_port;
static uint32_t gm_next_seq;

static uint32_t gm_sum_add(uint32_t sum,const uint8_t *data,uint16_t len) {
  while (len>1U) {
    sum+=((uint16_t)data[0]<<8)|data[1];
    data+=2;
    len-=2;
  }
  if (len) sum+=(uint16_t)data[0]<<8;
  return sum;
}

static uint16_t gm_sum_finish(uint32_t sum) {
  while (sum>>16) sum=(sum&0xffffU)+(sum>>16);
  return (uint16_t)~sum;
}

static uint16_t gm_tcp_checksum(const uint8_t src[4],const uint8_t dst[4],const uint8_t *data,uint16_t len) {
  uint8_t pseudo[4];
  uint32_t sum;

  pseudo[0]=0;
  pseudo[1]=GM_PROTO_TCP;
  pseudo[2]=(uint8_t)(len>>8);
  pseudo[3]=(uint8_t)len;
  sum=0;
  sum=gm_sum_add(sum,src,4);
  sum=gm_sum_add(sum,dst,4);
  sum=gm_sum_add(sum,pseudo,4);
  sum=gm_sum_add(sum,data,len);
  return gm_sum_finish(sum);
}

static int gm_tcp_segment(uint32_t seq,uint32_t ack,uint8_t flags,const uint8_t *data,uint16_t len) {
  uint8_t packet[GM_NET_MTU];
  struct gm_tcp_header *hdr;
  uint16_t total;

  total=(uint16_t)(sizeof(struct gm_tcp_header)+len);
  if (total>sizeof(packet)) return 0;
  hdr=(struct gm_tcp_header *)packet;
  hdr->src=gm_swap16(gm_tcp_src_port);
  hdr->dst=gm_swap16(gm_tcp_dst_port);
  hdr->seq=gm_swap32(seq);
  hdr->ack=gm_swap32(ack);
  hdr->offset=5U<<4;
  hdr->flags=flags;
  hdr->window=gm_swap16(GM_TCP_RX_SIZE);
  hdr->checksum=0;
  hdr->urgent=0;
  if (len) gm_memcpy(packet+sizeof(*hdr),data,len);
  hdr->checksum=gm_swap16(gm_tcp_checksum(gm_config.ip,gm_tcp_dst,packet,total));
  if (!gm_ipv4_send(gm_tcp_dst,GM_PROTO_TCP,packet,total)) return 0;
  gm_tcp_tx_count++;
  return 1;
}

void gm_tcp_init(void) {
  gm_tcp_state=GM_TCP_CLOSED;
  gm_tcp_rx_head=0;
  gm_tcp_rx_len=0;
  gm_tcp_unacked_len=0;
  gm_tcp_tx_count=0;
  gm_tcp_rx_count=0;
  gm_tcp_drop_count=0;
  gm_next_port=40000;
  gm_next_seq=0x474d0001U;
}

int gm_tcp_connect(const uint8_t dst[4],uint16_t port,uint64_t timeout) {
  uint64_t start;
  uint64_t last;
  uint8_t retry;

  if (!gm_net_prepare(dst,timeout)) return 0;
  gm_copy_ip(gm_tcp_dst,dst);
  gm_tcp_src_port=gm_next_port++;
  gm_tcp_dst_port=port;
  gm_tcp_seq=gm_next_seq;
  gm_next_seq+=0x1000U;
  gm_tcp_ack=0;
  gm_tcp_rx_head=0;
  gm_tcp_rx_len=0;
  gm_tcp_unacked_len=0;
  gm_tcp_state=GM_TCP_SYN;
  start=gm_ticks();
  last=start-timeout;
  retry=0;
  while (gm_ticks()-start<timeout) {
    if (gm_ticks()-last>=50U) {
      if (retry>=5U) break;
      if (!gm_tcp_segment(gm_tcp_seq,0,GM_TCP_FLAG_SYN,0,0)) return 0;
      last=gm_ticks();
      retry++;
    }
    gm_pump();
    if (gm_tcp_state==GM_TCP_ESTABLISHED) return 1;
    if (gm_tcp_state==GM_TCP_RESET) break;
  }
  gm_tcp_state=GM_TCP_CLOSED;
  return 0;
}

static int gm_tcp_append(const uint8_t *data,uint16_t len) {
  uint16_t room;
  uint16_t tail;
  uint16_t first;

  room=(uint16_t)(GM_TCP_RX_SIZE-gm_tcp_rx_len);
  if (len>room) {
    gm_tcp_drop_count+=len;
    return 0;
  }
  tail=(uint16_t)((gm_tcp_rx_head+gm_tcp_rx_len)%GM_TCP_RX_SIZE);
  first=len;
  if (first>GM_TCP_RX_SIZE-tail) first=(uint16_t)(GM_TCP_RX_SIZE-tail);
  if (first) gm_memcpy(gm_tcp_rx+tail,data,first);
  if (len>first) gm_memcpy(gm_tcp_rx,data+first,(uint16_t)(len-first));
  gm_tcp_rx_len=(uint16_t)(gm_tcp_rx_len+len);
  return 1;
}

void gm_tcp_input(const uint8_t src[4],const uint8_t *data,uint16_t len) {
  const struct gm_tcp_header *hdr;
  uint16_t header_len;
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t payload_len;
  uint32_t seq;
  uint32_t ack;
  uint8_t flags;
  const uint8_t *payload;
  uint16_t received_sum;

  if (len<sizeof(struct gm_tcp_header)) return;
  if (!gm_ip_eq(src,gm_tcp_dst)) return;
  hdr=(const struct gm_tcp_header *)data;
  header_len=(uint16_t)(hdr->offset>>4)*4U;
  if (header_len<sizeof(*hdr) || header_len>len) return;
  src_port=gm_swap16(hdr->src);
  dst_port=gm_swap16(hdr->dst);
  if (src_port!=gm_tcp_dst_port || dst_port!=gm_tcp_src_port) return;
  received_sum=gm_tcp_checksum(src,gm_config.ip,data,len);
  if (received_sum!=0) return;
  seq=gm_swap32(hdr->seq);
  ack=gm_swap32(hdr->ack);
  flags=hdr->flags;
  payload=data+header_len;
  payload_len=(uint16_t)(len-header_len);
  gm_tcp_rx_count++;

  if (flags&GM_TCP_FLAG_RST) {
    gm_tcp_state=GM_TCP_RESET;
    gm_tcp_unacked_len=0;
    return;
  }
  if (gm_tcp_state==GM_TCP_SYN && (flags&(GM_TCP_FLAG_SYN|GM_TCP_FLAG_ACK))==(GM_TCP_FLAG_SYN|GM_TCP_FLAG_ACK)) {
    if (ack!=gm_tcp_seq+1U) return;
    gm_tcp_seq++;
    gm_tcp_ack=seq+1U;
    if (gm_tcp_segment(gm_tcp_seq,gm_tcp_ack,GM_TCP_FLAG_ACK,0,0)) gm_tcp_state=GM_TCP_ESTABLISHED;
    return;
  }
  if (gm_tcp_state!=GM_TCP_ESTABLISHED && gm_tcp_state!=GM_TCP_FIN) return;
  if ((flags&GM_TCP_FLAG_ACK) && gm_tcp_unacked_len && ack>=gm_tcp_unacked_seq+gm_tcp_unacked_len) {
    gm_tcp_unacked_len=0;
    gm_tcp_unacked_retry=0;
  }
  if (payload_len) {
    if (seq==gm_tcp_ack && gm_tcp_append(payload,payload_len)) gm_tcp_ack+=payload_len;
    (void)gm_tcp_segment(gm_tcp_seq,gm_tcp_ack,GM_TCP_FLAG_ACK,0,0);
  }
  if (flags&GM_TCP_FLAG_FIN) {
    if (seq+payload_len==gm_tcp_ack) gm_tcp_ack++;
    (void)gm_tcp_segment(gm_tcp_seq,gm_tcp_ack,GM_TCP_FLAG_ACK,0,0);
    gm_tcp_state=GM_TCP_CLOSED;
    gm_tcp_unacked_len=0;
  }
}

void gm_tcp_poll(void) {
  uint64_t now;

  if (gm_tcp_state!=GM_TCP_ESTABLISHED || !gm_tcp_unacked_len) return;
  now=gm_ticks();
  if (now-gm_tcp_unacked_tick<50U) return;
  if (gm_tcp_unacked_retry>=5U) {
    gm_tcp_state=GM_TCP_RESET;
    gm_tcp_unacked_len=0;
    return;
  }
  gm_tcp_unacked_retry++;
  gm_tcp_unacked_tick=now;
  (void)gm_tcp_segment(gm_tcp_unacked_seq,gm_tcp_ack,GM_TCP_FLAG_ACK|GM_TCP_FLAG_PSH,
                       gm_tcp_unacked,gm_tcp_unacked_len);
}

int gm_tcp_send(const uint8_t *data,uint16_t len,uint64_t timeout) {
  uint64_t start;

  if (gm_tcp_state!=GM_TCP_ESTABLISHED || !data || !len || len>GM_TCP_TX_MAX) return 0;
  if (gm_tcp_unacked_len) {
    start=gm_ticks();
    while (gm_tcp_unacked_len && gm_ticks()-start<timeout) gm_pump();
    if (gm_tcp_unacked_len) {
      gm_tcp_state=GM_TCP_RESET;
      gm_tcp_unacked_len=0;
      return 0;
    }
  }
  gm_memcpy(gm_tcp_unacked,data,len);
  gm_tcp_unacked_len=len;
  gm_tcp_unacked_seq=gm_tcp_seq;
  gm_tcp_unacked_tick=gm_ticks();
  gm_tcp_unacked_retry=0;
  if (!gm_tcp_segment(gm_tcp_seq,gm_tcp_ack,GM_TCP_FLAG_ACK|GM_TCP_FLAG_PSH,data,len)) {
    gm_tcp_unacked_len=0;
    return 0;
  }
  gm_tcp_seq+=len;
  start=gm_ticks();
  while (gm_tcp_unacked_len && gm_ticks()-start<timeout) gm_pump();
  if (gm_tcp_unacked_len) {
    gm_tcp_state=GM_TCP_RESET;
    gm_tcp_unacked_len=0;
    return 0;
  }
  return gm_tcp_state==GM_TCP_ESTABLISHED;
}

uint16_t gm_tcp_available(void) {
  return gm_tcp_rx_len;
}

uint16_t gm_tcp_read(uint8_t *dst,uint16_t max) {
  uint16_t take;
  uint16_t first;

  if (!dst || !max) return 0;
  take=gm_tcp_rx_len;
  if (take>max) take=max;
  first=take;
  if (first>GM_TCP_RX_SIZE-gm_tcp_rx_head) first=(uint16_t)(GM_TCP_RX_SIZE-gm_tcp_rx_head);
  if (first) gm_memcpy(dst,gm_tcp_rx+gm_tcp_rx_head,first);
  if (take>first) gm_memcpy(dst+first,gm_tcp_rx,(uint16_t)(take-first));
  gm_tcp_rx_head=(uint16_t)((gm_tcp_rx_head+take)%GM_TCP_RX_SIZE);
  gm_tcp_rx_len=(uint16_t)(gm_tcp_rx_len-take);
  if (!gm_tcp_rx_len) gm_tcp_rx_head=0;
  return take;
}

void gm_tcp_drop(void) {
  gm_tcp_rx_head=0;
  gm_tcp_rx_len=0;
}

int gm_tcp_connected(void) {
  return gm_tcp_state==GM_TCP_ESTABLISHED;
}

void gm_tcp_close(void) {
  uint64_t start;

  if (gm_tcp_state!=GM_TCP_ESTABLISHED) return;
  start=gm_ticks();
  while (gm_tcp_unacked_len && gm_ticks()-start<100U) gm_pump();
  if (!gm_tcp_unacked_len && gm_tcp_segment(gm_tcp_seq,gm_tcp_ack,GM_TCP_FLAG_FIN|GM_TCP_FLAG_ACK,0,0)) {
    gm_tcp_seq++;
    gm_tcp_state=GM_TCP_FIN;
  }
}

void gm_tcp_status(void) {
  gm_write("tcp state=");
  if (gm_tcp_state==GM_TCP_CLOSED) gm_write("closed");
  else if (gm_tcp_state==GM_TCP_SYN) gm_write("syn");
  else if (gm_tcp_state==GM_TCP_ESTABLISHED) gm_write("up");
  else if (gm_tcp_state==GM_TCP_FIN) gm_write("fin");
  else gm_write("reset");
  gm_write(" rx=");
  gm_print_u64(gm_tcp_rx_count);
  gm_write(" tx=");
  gm_print_u64(gm_tcp_tx_count);
  gm_write(" buffered=");
  gm_print_u64(gm_tcp_rx_len);
  gm_write(" dropped=");
  gm_print_u64(gm_tcp_drop_count);
  gm_write("\n");
}

static int gm_store_path(const char *path) {
  uint64_t i;

  if (!path || path[0]!='/' || !path[1]) return 0;
  for (i=1;path[i];i++) {
    if (path[i]==' ' || path[i]=='\n' || path[i]=='\r' || path[i]=='\t') return 0;
    if (path[i]=='.' && path[i+1]=='.') return 0;
  }
  return i<192U;
}

static uint16_t gm_store_dec(char *buf,uint64_t value) {
  char tmp[24];
  uint16_t n;
  uint16_t out;

  if (!value) {
    buf[0]='0';
    return 1;
  }
  n=0;
  while (value) {
    tmp[n++]=(char)('0'+value%10ULL);
    value/=10ULL;
  }
  out=0;
  while (n) buf[out++]=tmp[--n];
  return out;
}

static uint16_t gm_store_line(char *buf,const char *op,const char *path,uint64_t a,int has_a) {
  uint16_t pos;
  uint16_t n;
  uint64_t i;

  pos=0;
  for (i=0;op[i];i++) buf[pos++]=op[i];
  if (path) {
    buf[pos++]=' ';
    for (i=0;path[i];i++) buf[pos++]=path[i];
  }
  if (has_a) {
    buf[pos++]=' ';
    n=gm_store_dec(buf+pos,a);
    pos=(uint16_t)(pos+n);
  }
  buf[pos++]='\n';
  return pos;
}

static int gm_store_wait_line(char *line,uint16_t cap,uint64_t timeout) {
  uint64_t start;
  uint16_t pos;
  uint8_t c;

  start=gm_ticks();
  pos=0;
  while (gm_ticks()-start<timeout) {
    gm_pump();
    while (gm_tcp_available()) {
      if (!gm_tcp_read(&c,1)) break;
      if (c=='\n') {
        if (pos && line[pos-1]=='\r') pos--;
        line[pos]=0;
        return 1;
      }
      if (pos+1U<cap) line[pos++]=(char)c;
      else return 0;
    }
  }
  return 0;
}

static int gm_store_ok(char *line,uint64_t timeout) {
  if (!gm_store_wait_line(line,256,timeout)) return 0;
  return gm_starts(line,"OK");
}

static int gm_store_number(const char *s,uint64_t *value) {
  uint64_t n;
  int digits;

  n=0;
  digits=0;
  while (*s>='0' && *s<='9') {
    uint64_t digit;

    digit=(uint64_t)(*s-'0');
    if (n>1844674407370955161ULL ||
        (n==1844674407370955161ULL && digit>5ULL)) return 0;
    n=n*10ULL+digit;
    digits++;
    s++;
  }
  if (!digits || *s) return 0;
  *value=n;
  return 1;
}

int gm_store_connect(void) {
  char line[256];
  int ok;

  gm_tcp_drop();
  if (!gm_tcp_connect(gm_config.store_ip,gm_config.store_port,300U)) return 0;
  ok=gm_store_wait_line(line,sizeof(line),200U) && gm_streq(line,"GMSTORE 2.0");
  if (!ok) gm_tcp_close();
  return ok;
}

int gm_store_ping(void) {
  char line[256];
  static const uint8_t req[]="PING\n";

  gm_tcp_drop();
  if (!gm_tcp_send(req,sizeof(req)-1U,200U)) return 0;
  if (!gm_store_wait_line(line,sizeof(line),200U)) return 0;
  return gm_streq(line,"OK PONG");
}

int gm_store_list_prefix(const char *prefix) {
  char line[256];
  static const uint8_t req[]="LIST\n";

  gm_tcp_drop();
  if (!gm_tcp_send(req,sizeof(req)-1U,200U)) return 0;
  for (;;) {
    if (!gm_store_wait_line(line,sizeof(line),200U)) return 0;
    if (gm_streq(line,"OK END")) return 1;
    if (!gm_starts(line,"ITEM ")) return 0;
    if (!prefix || gm_starts(line+5,prefix)) {
      gm_write(line+5);
      gm_write("\n");
    }
  }
}

int gm_store_list(void) {
  return gm_store_list_prefix(0);
}

int gm_store_size(const char *path,uint64_t *size) {
  char req[256];
  char line[256];
  uint16_t len;

  if (!size || !gm_store_path(path)) return 0;
  len=gm_store_line(req,"STAT",path,0,0);
  gm_tcp_drop();
  if (!gm_tcp_send((const uint8_t *)req,len,200U)) return 0;
  if (!gm_store_wait_line(line,sizeof(line),200U) || !gm_starts(line,"OK ")) return 0;
  return gm_store_number(line+3,size);
}

int gm_store_read(const char *path,uint64_t offset,uint8_t *data,uint16_t len,uint16_t *got) {
  char req[256];
  char line[256];
  uint64_t count;
  uint64_t start;
  uint16_t req_len;
  uint16_t have;

  if (!got || !gm_store_path(path) || (!data && len) || len>GM_STORE_BLOCK) return 0;
  req_len=gm_store_line(req,"READ",path,offset,1);
  req[req_len-1]=' ';
  req_len=(uint16_t)(req_len+gm_store_dec(req+req_len,len));
  req[req_len++]='\n';
  gm_tcp_drop();
  if (!gm_tcp_send((const uint8_t *)req,req_len,200U)) return 0;
  if (!gm_store_wait_line(line,sizeof(line),200U) || !gm_starts(line,"OK ")) return 0;
  if (!gm_store_number(line+3,&count) || count>len) return 0;
  have=0;
  start=gm_ticks();
  while (have<count && gm_ticks()-start<300U) {
    gm_pump();
    have=(uint16_t)(have+gm_tcp_read(data+have,(uint16_t)(count-have)));
  }
  if (have!=count) return 0;
  *got=have;
  return 1;
}

int gm_store_stat(const char *path) {
  uint64_t size;

  if (!gm_store_size(path,&size)) return 0;
  gm_write(path);
  gm_write(" ");
  gm_print_u64(size);
  gm_write(" bytes\n");
  return 1;
}

int gm_store_cat(const char *path) {
  uint8_t data[GM_STORE_BLOCK];
  uint64_t offset;
  uint16_t got;
  uint16_t i;

  if (!gm_store_path(path)) return 0;
  offset=0;
  for (;;) {
    if (!gm_store_read(path,offset,data,GM_STORE_BLOCK,&got)) return 0;
    for (i=0;i<got;i++) gm_putc((char)data[i]);
    offset+=got;
    if (got<GM_STORE_BLOCK) return 1;
  }
}

int gm_store_write(const char *path,const uint8_t *data,uint16_t len,int append) {
  uint8_t req[256+GM_STORE_BLOCK];
  char line[256];
  uint16_t pos;
  uint16_t n;
  uint64_t i;
  const char *op;

  if (!gm_store_path(path) || (!data && len) || len>GM_STORE_BLOCK) return 0;
  op=append?"APPEND":"WRITE";
  pos=0;
  for (i=0;op[i];i++) req[pos++]=(uint8_t)op[i];
  req[pos++]=' ';
  for (i=0;path[i];i++) req[pos++]=(uint8_t)path[i];
  req[pos++]=' ';
  n=gm_store_dec((char *)req+pos,len);
  pos=(uint16_t)(pos+n);
  req[pos++]='\n';
  if ((uint32_t)pos+len>sizeof(req) || (uint32_t)pos+len>GM_TCP_TX_MAX) return 0;
  if (len) gm_memcpy(req+pos,data,len);
  pos=(uint16_t)(pos+len);
  gm_tcp_drop();
  if (!gm_tcp_send(req,pos,300U)) return 0;
  return gm_store_ok(line,300U);
}

int gm_store_delete(const char *path) {
  char req[256];
  char line[256];
  uint16_t len;

  if (!gm_store_path(path)) return 0;
  len=gm_store_line(req,"DEL",path,0,0);
  gm_tcp_drop();
  if (!gm_tcp_send((const uint8_t *)req,len,200U)) return 0;
  return gm_store_ok(line,200U);
}

void gm_store_status(void) {
  gm_write("store server=");
  gm_print_ip(gm_config.store_ip);
  gm_write(":");
  gm_print_u64(gm_config.store_port);
  gm_write(" block=");
  gm_print_u64(GM_STORE_BLOCK);
  gm_write(" connected=");
  gm_write(gm_tcp_connected()?"yes":"no");
  gm_write("\n");
}
