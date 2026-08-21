// Gianluca Mazzini @2026- Version 1.0
#include "../gmprog.h"

static int is_prime(uint64_t n) {
  uint64_t d;

  if (n<2ULL) return 0;
  if (n==2ULL) return 1;
  if (!(n&1ULL)) return 0;
  for (d=3ULL;d<=n/d;d+=2ULL) {
    if (!(n%d)) return 0;
  }
  return 1;
}

int gm_main(const char *args,uint64_t arg_len) {
  uint64_t n;
  uint64_t count;

  (void)args;
  (void)arg_len;
  n=2ULL;
  count=0ULL;
  for (;count<100ULL;n++) {
    if (!is_prime(n)) continue;
    gm_print_u64(n);
    count++;
    gm_write(count<100ULL?" ":"\n");
  }
  return 0;
}
