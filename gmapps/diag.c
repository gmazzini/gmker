// Gianluca Mazzini @2026- Version 1.02
#include "../gmprog.h"

static void print_ip(const uint8_t ip[4]) {
  uint64_t i;

  for (i=0ULL;i<4ULL;i++) {
    gm_print_u64(ip[i]);
    if (i<3ULL) gm_write(".");
  }
}

int gm_main(const char *args,uint64_t arg_len) {
  static const char path[]="/diag.txt";
  static const char first[]="diag";
  static const char second[]="-ok";
  uint8_t gateway[4];
  char data[8];
  uint16_t got;
  uint64_t now;
  uint64_t store_size;
  uint64_t i;
  int ok;
  int released;

  gm_write("GMKER application diagnostic\n");
  gm_write("args: ");
  if (arg_len) gm_write(args);
  else gm_write("(none)");
  gm_write("\n");

  now=gm_ticks();
  gm_write("ticks: ");
  gm_print_u64(now);
  gm_write("\n");

  ok=gm_gateway(gateway);
  gm_write("gateway: ");
  if (ok) print_ip(gateway);
  else gm_write("error");
  gm_write("\n");
  if (!ok) return 1;

  gm_write("ping gateway: ");
  ok=gm_ping(gateway,300ULL);
  gm_write(ok?"ok\n":"error\n");
  if (!ok) return 2;

  gm_write("tcp acquire: ");
  ok=gm_resource_acquire(GM_RESOURCE_TCP);
  gm_write(ok?"ok\n":"error\n");
  if (!ok) return 6;

  gm_write("store write: ");
  ok=gm_store_write(path,first,4U);
  gm_write(ok?"ok\n":"error\n");
  if (!ok) {
    (void)gm_resource_release(GM_RESOURCE_TCP);
    return 3;
  }

  gm_write("store append: ");
  ok=gm_store_append(path,second,3U);
  gm_write(ok?"ok\n":"error\n");
  if (!ok) {
    (void)gm_resource_release(GM_RESOURCE_TCP);
    return 4;
  }

  gm_write("store stat: ");
  ok=gm_store_stat(path,&store_size) && store_size==7ULL;
  gm_write(ok?"ok\n":"error\n");
  if (!ok) {
    (void)gm_resource_release(GM_RESOURCE_TCP);
    return 8;
  }

  for (i=0ULL;i<sizeof(data);i++) data[i]=0;
  got=gm_store_read(path,0ULL,data,7U);
  released=gm_resource_release(GM_RESOURCE_TCP);
  ok=got==7U && data[0]=='d' && data[1]=='i' && data[2]=='a' && data[3]=='g' &&
     data[4]=='-' && data[5]=='o' && data[6]=='k';
  gm_write("store read: ");
  gm_write(ok?"ok\n":"error\n");
  gm_write("tcp release: ");
  gm_write(released?"ok\n":"error\n");
  if (!ok) return 5;
  if (!released) return 7;

  gm_write("all tests passed\n");
  return 0;
}
