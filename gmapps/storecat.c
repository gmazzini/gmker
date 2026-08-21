// Gianluca Mazzini @2026- Version 1.02
#include "../gmprog.h"

int gm_main(const char *args,uint64_t arg_len) {
  char data[1024];
  uint64_t offset;
  uint64_t size;
  uint64_t remain;
  uint16_t got;
  uint16_t want;
  int released;

  if (!arg_len) {
    gm_write("usage: run storecat /path\n");
    return 1;
  }
  if (!gm_resource_acquire(GM_RESOURCE_TCP)) {
    gm_write("storecat: tcp busy\n");
    return 4;
  }
  if (!gm_store_stat(args,&size)) {
    (void)gm_resource_release(GM_RESOURCE_TCP);
    gm_write("storecat: stat error\n");
    return 2;
  }
  offset=0ULL;
  for (;offset<size;offset+=got) {
    remain=size-offset;
    want=remain>1023ULL?1023U:(uint16_t)remain;
    got=gm_store_read(args,offset,data,want);
    if (got!=want) {
      (void)gm_resource_release(GM_RESOURCE_TCP);
      gm_write("storecat: read error\n");
      return 2;
    }
    data[got]=0;
    if (!gm_write(data)) {
      (void)gm_resource_release(GM_RESOURCE_TCP);
      return 3;
    }
  }
  released=gm_resource_release(GM_RESOURCE_TCP);
  if (!released) return 5;
  return 0;
}
