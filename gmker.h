// Gianluca Mazzini @2026- Version 2.0
#ifndef GMKER_H
#define GMKER_H

#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "gmabi.h"

#define GM_VERSION "3.0"
#define GM_TICK_HZ 100U
#define GM_PAGE_SIZE 4096ULL
#define GM_NET_MTU 1500U
#define GM_TCP_RX_SIZE 4096U
#define GM_TCP_TX_MAX 1280U
#define GM_ARCH_NATIVE GM_ARCH_X86_64
#define GM_ARCH_NAME "x86_64"
#define GM_PROGRAM_ARG_SIZE 4096ULL
#define GM_PROGRAM_STACK_SIZE 16384ULL
#define GM_PROGRAM_GUARD_SIZE 4096ULL
#define GM_PROGRAM_ARG (GM_PROGRAM_BASE+GM_PROGRAM_IMAGE_MAX)
#define GM_PROGRAM_GUARD (GM_PROGRAM_ARG+GM_PROGRAM_ARG_SIZE)
#define GM_PROGRAM_STACK (GM_PROGRAM_GUARD+GM_PROGRAM_GUARD_SIZE)
#define GM_PROGRAM_END (GM_PROGRAM_STACK+GM_PROGRAM_STACK_SIZE)
#define GM_PROGRAM_MAX_TICKS 1000ULL

struct gm_config {
  uint8_t ip[4];
  uint8_t mask[4];
  uint8_t gateway[4];
  uint8_t store_ip[4];
  uint16_t store_port;
};

extern struct gm_config gm_config;

static inline void gm_outb(uint16_t port,uint8_t value) {
  __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port));
}

static inline void gm_outw(uint16_t port,uint16_t value) {
  __asm__ volatile("outw %0,%1"::"a"(value),"Nd"(port));
}

static inline void gm_outl(uint16_t port,uint32_t value) {
  __asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));
}

static inline uint8_t gm_inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1,%0":"=a"(value):"Nd"(port));
  return value;
}

static inline uint16_t gm_inw(uint16_t port) {
  uint16_t value;
  __asm__ volatile("inw %1,%0":"=a"(value):"Nd"(port));
  return value;
}

static inline uint32_t gm_inl(uint16_t port) {
  uint32_t value;
  __asm__ volatile("inl %1,%0":"=a"(value):"Nd"(port));
  return value;
}

void gm_memcpy(void *dst,const void *src,uint64_t len);
void gm_memset(void *dst,uint8_t value,uint64_t len);
int gm_memeq(const void *a,const void *b,uint64_t len);
uint64_t gm_strlen(const char *s);
int gm_streq(const char *a,const char *b);
int gm_starts(const char *s,const char *prefix);
uint64_t gm_parse_u64(const char *s,int *ok);
int gm_parse_ipv4(const char *s,uint8_t ip[4],const char **end);
void gm_copy_ip(uint8_t dst[4],const uint8_t src[4]);
int gm_ip_eq(const uint8_t a[4],const uint8_t b[4]);
uint16_t gm_swap16(uint16_t value);
uint32_t gm_swap32(uint32_t value);

void gm_serial_init(void);
int gm_serial_getc(void);
void gm_putc(char c);
void gm_write(const char *s);
void gm_print_u64(uint64_t value);
void gm_print_hex(uint64_t value);
void gm_print_ip(const uint8_t ip[4]);
void gm_panic(const char *message);
void gm_shutdown(void);

void gm_memory_init(struct limine_memmap_response *memmap,uint64_t hhdm);
uint64_t gm_page_alloc(void);
void *gm_phys(uint64_t phys);
uint64_t gm_memory_free(void);
uint64_t gm_address_space_current(void);
uint64_t gm_address_space_create(void);
void gm_address_space_switch(uint64_t cr3);
int gm_map_page(uint64_t virt,uint64_t phys,uint64_t flags);
int gm_map_user_page_in(uint64_t cr3,uint64_t virt,uint64_t phys,uint64_t flags);
uint64_t gm_read_cr2(void);

void gm_irq_init(void);
void gm_timer_init(void);
uint64_t gm_ticks(void);
void gm_timer_irq(void);
void gm_fault_dispatch(uint64_t vector,uint64_t code,uint64_t cs);

int gm_virtio_net_init(uint8_t mac[6]);
int gm_virtio_net_send(const uint8_t *frame,uint16_t len);
int gm_virtio_net_poll(uint8_t *frame,uint16_t cap,uint16_t *len);

void gm_net_init(void);
void gm_net_poll(void);
void gm_net_status(void);
void gm_arp_status(void);
int gm_net_prepare(const uint8_t dst[4],uint64_t timeout);
int gm_ipv4_send(const uint8_t dst[4],uint8_t protocol,const uint8_t *data,uint16_t len);
uint16_t gm_checksum(const void *data,uint16_t len);
int gm_ping(const uint8_t dst[4],uint64_t timeout);
void gm_ipv4_input(const uint8_t src[4],uint8_t protocol,const uint8_t *data,uint16_t len);

void gm_tcp_init(void);
void gm_tcp_poll(void);
void gm_tcp_input(const uint8_t src[4],const uint8_t *data,uint16_t len);
int gm_tcp_connect(const uint8_t dst[4],uint16_t port,uint64_t timeout);
int gm_tcp_send(const uint8_t *data,uint16_t len,uint64_t timeout);
uint16_t gm_tcp_available(void);
uint16_t gm_tcp_read(uint8_t *dst,uint16_t max);
void gm_tcp_drop(void);
int gm_tcp_connected(void);
void gm_tcp_close(void);
void gm_tcp_status(void);

int gm_store_connect(void);
int gm_store_ping(void);
int gm_store_list(void);
int gm_store_list_prefix(const char *prefix);
int gm_store_size(const char *path,uint64_t *size);
int gm_store_read(const char *path,uint64_t offset,uint8_t *data,uint16_t len,uint16_t *got);
int gm_store_stat(const char *path);
int gm_store_cat(const char *path);
int gm_store_write(const char *path,const uint8_t *data,uint16_t len,int append);
int gm_store_delete(const char *path);
void gm_store_status(void);

void gm_programs_init(void);
void gm_programs_list(void);
void gm_programs_status(void);
void gm_resources_status(void);
int gm_program_run(const char *name,const char *args);
void gm_program_resume_user(void *frame);
int gm_program_schedule(void);
int gm_program_service(void *frame);
int gm_program_fault(uint64_t vector,uint64_t code,uint64_t cs);
int gm_program_preempt(void *frame);

void gm_shell_init(void);
void gm_shell_poll(void);
void gm_pump(void);

#endif
