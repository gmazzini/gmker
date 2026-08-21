// Gianluca Mazzini @2026- Version 2.04
#include "gmker.h"

struct gm_config gm_config;

__attribute__((used,section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used,section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used,section(".limine_requests")))
static volatile struct limine_memmap_request gm_memmap_request={
  .id=LIMINE_MEMMAP_REQUEST,
  .revision=0
};

__attribute__((used,section(".limine_requests")))
static volatile struct limine_hhdm_request gm_hhdm_request={
  .id=LIMINE_HHDM_REQUEST,
  .revision=0
};

__attribute__((used,section(".limine_requests")))
static volatile struct limine_executable_cmdline_request gm_cmdline_request={
  .id=LIMINE_EXECUTABLE_CMDLINE_REQUEST,
  .revision=0
};

__attribute__((used,section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

static void gm_defaults(void) {
  static const uint8_t ip[4]={10,0,2,15};
  static const uint8_t mask[4]={255,255,255,0};
  static const uint8_t gateway[4]={10,0,2,2};
  static const uint8_t store[4]={10,0,2,2};

  gm_copy_ip(gm_config.ip,ip);
  gm_copy_ip(gm_config.mask,mask);
  gm_copy_ip(gm_config.gateway,gateway);
  gm_copy_ip(gm_config.store_ip,store);
  gm_config.store_port=7070;
}

static const char *gm_arg(const char *cmdline,const char *name) {
  const char *s;
  uint64_t n;

  if (!cmdline) return 0;
  n=gm_strlen(name);
  s=cmdline;
  for (;;) {
    while (*s==' ' || *s=='\t') s++;
    if (!*s) return 0;
    if (gm_starts(s,name) && s[n]=='=') return s+n+1;
    while (*s && *s!=' ' && *s!='\t') s++;
  }
}

static int gm_parse_prefix(const char **ps,uint8_t mask[4]) {
  const char *s;
  uint32_t prefix;
  int digits;
  int i;

  s=*ps;
  prefix=0;
  digits=0;
  while (*s>='0' && *s<='9') {
    prefix=prefix*10U+(uint32_t)(*s-'0');
    digits++;
    if (prefix>32U) return 0;
    s++;
  }
  if (!digits) return 0;
  for (i=0;i<4;i++) {
    if (prefix>=8U) {
      mask[i]=255;
      prefix-=8U;
    } else if (prefix) {
      mask[i]=(uint8_t)(0xffU<<(8U-prefix));
      prefix=0;
    } else {
      mask[i]=0;
    }
  }
  *ps=s;
  return 1;
}

static void gm_config_net(const char *arg) {
  const char *s;
  uint8_t ip[4];
  uint8_t mask[4];
  uint8_t gateway[4];

  if (!arg) return;
  s=arg;
  if (!gm_parse_ipv4(s,ip,&s) || *s!='/') return;
  s++;
  if (!gm_parse_prefix(&s,mask) || *s!=',') return;
  s++;
  if (!gm_parse_ipv4(s,gateway,&s)) return;
  if (*s && *s!=' ' && *s!='\t') return;
  gm_copy_ip(gm_config.ip,ip);
  gm_copy_ip(gm_config.mask,mask);
  gm_copy_ip(gm_config.gateway,gateway);
}

static void gm_config_store(const char *arg) {
  const char *s;
  uint8_t ip[4];
  uint64_t port;
  int ok;

  if (!arg) return;
  s=arg;
  if (!gm_parse_ipv4(s,ip,&s) || *s!=':') return;
  s++;
  port=gm_parse_u64(s,&ok);
  if (!ok || !port || port>65535U) return;
  while (*s>='0' && *s<='9') s++;
  if (*s && *s!=' ' && *s!='\t') return;
  gm_copy_ip(gm_config.store_ip,ip);
  gm_config.store_port=(uint16_t)port;
}

static void gm_load_config(void) {
  const char *cmdline;

  gm_defaults();
  cmdline=0;
  if (gm_cmdline_request.response) cmdline=gm_cmdline_request.response->cmdline;
  gm_config_net(gm_arg(cmdline,"gmker.net"));
  gm_config_store(gm_arg(cmdline,"gmker.store"));
}

void gm_pump(void) {
  gm_net_poll();
  gm_tcp_poll();
  __asm__ volatile("hlt");
}

void kernel_main(void) {
  gm_serial_init();
  gm_write("\nGMKER 3.1\n");

  if (!LIMINE_BASE_REVISION_SUPPORTED) gm_panic("unsupported Limine base revision");
  if (!gm_memmap_request.response) gm_panic("no memory map");
  if (!gm_hhdm_request.response) gm_panic("no HHDM");

  gm_load_config();
  gm_memory_init(gm_memmap_request.response,gm_hhdm_request.response->offset);
  gm_irq_init();
  gm_timer_init();
  gm_net_init();
  gm_tcp_init();
  gm_programs_init();
  gm_shell_init();

  __asm__ volatile("sti");
  for (;;) {
    gm_net_poll();
    gm_tcp_poll();
    gm_shell_poll();
    if (!gm_program_schedule()) __asm__ volatile("hlt");
  }
}
