// Gianluca Mazzini @2026- Version 1.0
#include "../gmprog.h"

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

static void print_ip(const uint8_t ip[4]) {
  uint64_t i;

  for (i=0ULL;i<4ULL;i++) {
    gm_print_u64(ip[i]);
    if (i<3ULL) gm_write(".");
  }
}

int gm_main(const char *args,uint64_t arg_len) {
  uint8_t ip[4];
  int ok;

  if (!parse_ip(args,arg_len,ip)) {
    gm_write("usage: run netping IPv4\n");
    return 1;
  }
  gm_write("ping ");
  print_ip(ip);
  gm_write(": ");
  ok=gm_ping(ip,300ULL);
  gm_write(ok?"ok\n":"timeout\n");
  return ok?0:2;
}
