// Gianluca Mazzini @2026- Version 2.3
#ifndef GMPROG_H
#define GMPROG_H

#include <stdint.h>
#include <stddef.h>
#include "gmabi.h"



#if defined(__x86_64__)
#define GM_ARCH_NATIVE GM_ARCH_X86_64
#else
#error gmprog service gate backend is not implemented for this architecture yet
#endif

static inline uint64_t gm_call0(uint64_t service) {
  uint64_t result;

  result=service;
  __asm__ volatile("int $0x80":"+a"(result)::"memory","cc");
  return result;
}

static inline uint64_t gm_call1(uint64_t service,uint64_t a) {
  uint64_t result;

  result=service;
  __asm__ volatile("int $0x80":"+a"(result):"D"(a):"memory","cc");
  return result;
}

static inline uint64_t gm_call2(uint64_t service,uint64_t a,uint64_t b) {
  uint64_t result;

  result=service;
  __asm__ volatile("int $0x80":"+a"(result):"D"(a),"S"(b):"memory","cc");
  return result;
}

static inline uint64_t gm_call3(uint64_t service,uint64_t a,uint64_t b,uint64_t c) {
  uint64_t result;

  result=service;
  __asm__ volatile("int $0x80":"+a"(result):"D"(a),"S"(b),"d"(c):"memory","cc");
  return result;
}

static inline uint64_t gm_call4(uint64_t service,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
  uint64_t result;

  result=service;
  __asm__ volatile("int $0x80":"+a"(result):"D"(a),"S"(b),"d"(c),"c"(d):"memory","cc");
  return result;
}

static inline uint64_t gm_call6(uint64_t service,uint64_t a,uint64_t b,uint64_t c,uint64_t d,
                                uint64_t e,uint64_t f) {
  register uint64_t r8 __asm__("r8");
  register uint64_t r9 __asm__("r9");
  uint64_t result;

  result=service;
  r8=e;
  r9=f;
  __asm__ volatile("int $0x80":"+a"(result):"D"(a),"S"(b),"d"(c),"c"(d),"r"(r8),"r"(r9):"memory","cc");
  return result;
}

static inline void gm_exit(int result) {
  (void)gm_call1(GM_SVC_EXIT,(uint64_t)(int64_t)result);
  for (;;) {}
}

static inline int gm_write(const char *text) {
  return (int)gm_call1(GM_SVC_WRITE,(uint64_t)text);
}

static inline int gm_print_u64(uint64_t value) {
  return (int)gm_call1(GM_SVC_PRINT_U64,value);
}

static inline uint64_t gm_ticks(void) {
  return gm_call0(GM_SVC_TICKS);
}

static inline int gm_gateway(uint8_t ip[4]) {
  return (int)gm_call1(GM_SVC_GATEWAY,(uint64_t)ip);
}

static inline int gm_ping(const uint8_t ip[4],uint64_t timeout) {
  return (int)gm_call2(GM_SVC_PING,(uint64_t)ip,timeout);
}

static inline uint16_t gm_store_read(const char *path,uint64_t offset,void *data,uint16_t len) {
  return (uint16_t)gm_call4(GM_SVC_STORE_READ,(uint64_t)path,offset,(uint64_t)data,len);
}

static inline int gm_store_stat(const char *path,uint64_t *size) {
  return (int)gm_call2(GM_SVC_STORE_STAT,(uint64_t)path,(uint64_t)size);
}

static inline int gm_store_write(const char *path,const void *data,uint16_t len) {
  return (int)gm_call3(GM_SVC_STORE_WRITE,(uint64_t)path,(uint64_t)data,len);
}

static inline int gm_store_append(const char *path,const void *data,uint16_t len) {
  return (int)gm_call3(GM_SVC_STORE_APPEND,(uint64_t)path,(uint64_t)data,len);
}

static inline uint16_t gm_udp_exchange(const uint8_t ip[4],uint16_t port,const void *tx,uint16_t tx_len,
                                       void *rx,uint16_t rx_max) {
  return (uint16_t)gm_call6(GM_SVC_UDP_EXCHANGE,(uint64_t)ip,port,(uint64_t)tx,tx_len,
                            (uint64_t)rx,rx_max);
}

static inline int gm_resource_acquire(uint32_t resource) {
  return (int)gm_call1(GM_SVC_RESOURCE_ACQUIRE,resource);
}

static inline int gm_resource_release(uint32_t resource) {
  return (int)gm_call1(GM_SVC_RESOURCE_RELEASE,resource);
}

#endif
