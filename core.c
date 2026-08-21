// Gianluca Mazzini @2026- Version 2.03
#include "gmker.h"

#define GM_COM1 0x3f8
#define GM_PIC1_CMD 0x20
#define GM_PIC1_DATA 0x21
#define GM_PIC2_CMD 0xa0
#define GM_PIC2_DATA 0xa1
#define GM_PIT_CH0 0x40
#define GM_PIT_CMD 0x43
#define GM_PIT_HZ 1193182U
#define GM_PTE_PRESENT 0x001ULL
#define GM_PTE_WRITE 0x002ULL
#define GM_PTE_USER 0x004ULL
#define GM_ADDR_MASK 0x000ffffffffff000ULL
#define GM_MIN_PHYS 0x1000000ULL

struct gm_idt_entry {
  uint16_t lo;
  uint16_t selector;
  uint8_t ist;
  uint8_t flags;
  uint16_t mid;
  uint32_t hi;
  uint32_t zero;
} __attribute__((packed));

struct gm_idt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

struct gm_tss {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist1;
  uint64_t ist2;
  uint64_t ist3;
  uint64_t ist4;
  uint64_t ist5;
  uint64_t ist6;
  uint64_t ist7;
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb;
} __attribute__((packed));

extern void gm_isr_de(void);
extern void gm_isr_db(void);
extern void gm_isr_bp(void);
extern void gm_isr_of(void);
extern void gm_isr_br(void);
extern void gm_isr_ud(void);
extern void gm_isr_nm(void);
extern void gm_isr_df(void);
extern void gm_isr_ts(void);
extern void gm_isr_np(void);
extern void gm_isr_ss(void);
extern void gm_isr_gp(void);
extern void gm_isr_pf(void);
extern void gm_isr_mf(void);
extern void gm_isr_ac(void);
extern void gm_isr_xm(void);
extern void gm_isr_timer(void);
extern void gm_isr_service(void);
extern void gm_gdt_load(const void *ptr);

static struct gm_idt_entry gm_idt[256];
static uint64_t gm_gdt[7];
static struct gm_tss gm_tss;
static uint8_t gm_ring0_stack[16384] __attribute__((aligned(16)));
static uint8_t gm_df_stack[8192] __attribute__((aligned(16)));
static volatile uint64_t gm_tick_count;
static uint64_t gm_hhdm;
static uint64_t gm_page_next;
static uint64_t gm_page_end;

void gm_memcpy(void *dst,const void *src,uint64_t len) {
  uint8_t *d;
  const uint8_t *s;
  uint64_t i;

  d=(uint8_t *)dst;
  s=(const uint8_t *)src;
  for (i=0;i<len;i++) d[i]=s[i];
}

void gm_memset(void *dst,uint8_t value,uint64_t len) {
  uint8_t *d;
  uint64_t i;

  d=(uint8_t *)dst;
  for (i=0;i<len;i++) d[i]=value;
}

int gm_memeq(const void *a,const void *b,uint64_t len) {
  const uint8_t *aa;
  const uint8_t *bb;
  uint64_t i;

  aa=(const uint8_t *)a;
  bb=(const uint8_t *)b;
  for (i=0;i<len;i++) if (aa[i]!=bb[i]) return 0;
  return 1;
}

uint64_t gm_strlen(const char *s) {
  uint64_t n;

  if (!s) return 0;
  n=0;
  while (s[n]) n++;
  return n;
}

int gm_streq(const char *a,const char *b) {
  if (!a || !b) return 0;
  while (*a && *b) {
    if (*a!=*b) return 0;
    a++;
    b++;
  }
  return *a==*b;
}

int gm_starts(const char *s,const char *prefix) {
  if (!s || !prefix) return 0;
  while (*prefix) {
    if (*s!=*prefix) return 0;
    s++;
    prefix++;
  }
  return 1;
}

uint64_t gm_parse_u64(const char *s,int *ok) {
  uint64_t value;
  uint64_t digit;
  int digits;

  value=0;
  digits=0;
  while (*s>='0' && *s<='9') {
    digit=(uint64_t)(*s-'0');
    if (value>1844674407370955161ULL ||
        (value==1844674407370955161ULL && digit>5ULL)) {
      if (ok) *ok=0;
      return 0;
    }
    value=value*10ULL+digit;
    digits++;
    s++;
  }
  if (ok) *ok=digits!=0;
  return value;
}

int gm_parse_ipv4(const char *s,uint8_t ip[4],const char **end) {
  uint32_t value;
  int digits;
  int i;

  if (!s) return 0;
  for (i=0;i<4;i++) {
    value=0;
    digits=0;
    while (*s>='0' && *s<='9') {
      value=value*10U+(uint32_t)(*s-'0');
      digits++;
      if (value>255U || digits>3) return 0;
      s++;
    }
    if (!digits) return 0;
    ip[i]=(uint8_t)value;
    if (i!=3) {
      if (*s!='.') return 0;
      s++;
    }
  }
  if (end) *end=s;
  return 1;
}

void gm_copy_ip(uint8_t dst[4],const uint8_t src[4]) {
  int i;
  for (i=0;i<4;i++) dst[i]=src[i];
}

int gm_ip_eq(const uint8_t a[4],const uint8_t b[4]) {
  return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

uint16_t gm_swap16(uint16_t value) {
  return (uint16_t)((value>>8)|(value<<8));
}

uint32_t gm_swap32(uint32_t value) {
  return ((value&0x000000ffU)<<24)|((value&0x0000ff00U)<<8)|
         ((value&0x00ff0000U)>>8)|((value&0xff000000U)>>24);
}

void gm_serial_init(void) {
  gm_outb(GM_COM1+1,0x00);
  gm_outb(GM_COM1+3,0x80);
  gm_outb(GM_COM1+0,0x01);
  gm_outb(GM_COM1+1,0x00);
  gm_outb(GM_COM1+3,0x03);
  gm_outb(GM_COM1+2,0xc7);
  gm_outb(GM_COM1+4,0x0b);
}

int gm_serial_getc(void) {
  int c;

  if (!(gm_inb(GM_COM1+5)&0x01)) return -1;
  c=(int)gm_inb(GM_COM1);
  if (c=='\r') c='\n';
  return c;
}

void gm_putc(char c) {
  uint32_t spin;

  gm_outb(0xe9,(uint8_t)c);
  for (spin=0;spin<100000U;spin++) {
    if (gm_inb(GM_COM1+5)&0x20) {
      gm_outb(GM_COM1,(uint8_t)c);
      return;
    }
    __asm__ volatile("pause");
  }
}

void gm_write(const char *s) {
  if (!s) return;
  while (*s) {
    if (*s=='\n') gm_putc('\r');
    gm_putc(*s++);
  }
}

void gm_print_u64(uint64_t value) {
  char buf[24];
  int n;

  if (!value) {
    gm_putc('0');
    return;
  }
  n=0;
  while (value && n<(int)sizeof(buf)) {
    buf[n++]=(char)('0'+value%10ULL);
    value/=10ULL;
  }
  while (n) gm_putc(buf[--n]);
}

void gm_print_hex(uint64_t value) {
  static const char hex[]="0123456789abcdef";
  int shift;

  gm_write("0x");
  for (shift=60;shift>=0;shift-=4) gm_putc(hex[(value>>shift)&0x0f]);
}

void gm_print_ip(const uint8_t ip[4]) {
  int i;
  for (i=0;i<4;i++) {
    gm_print_u64(ip[i]);
    if (i!=3) gm_putc('.');
  }
}

void gm_panic(const char *message) {
  __asm__ volatile("cli");
  gm_write("\nPANIC: ");
  gm_write(message);
  gm_write("\n");
  for (;;) __asm__ volatile("hlt");
}

void gm_shutdown(void) {
  gm_write("shutdown\n");
  gm_outl(0x501,0x10U);
  for (;;) __asm__ volatile("cli; hlt");
}

static uint64_t gm_align_up(uint64_t value,uint64_t align) {
  return (value+align-1ULL)&~(align-1ULL);
}

void gm_memory_init(struct limine_memmap_response *memmap,uint64_t hhdm) {
  struct limine_memmap_entry *entry;
  uint64_t best_base;
  uint64_t best_end;
  uint64_t base;
  uint64_t end;
  uint64_t i;

  gm_hhdm=hhdm;
  best_base=0;
  best_end=0;
  for (i=0;i<memmap->entry_count;i++) {
    entry=memmap->entries[i];
    if (entry->type!=LIMINE_MEMMAP_USABLE) continue;
    base=entry->base;
    end=entry->base+entry->length;
    if (end<=GM_MIN_PHYS) continue;
    if (base<GM_MIN_PHYS) base=GM_MIN_PHYS;
    base=gm_align_up(base,GM_PAGE_SIZE);
    if (end<=base) continue;
    if (!best_base || end-base>best_end-best_base) {
      best_base=base;
      best_end=end&~(GM_PAGE_SIZE-1ULL);
    }
  }
  if (!best_base || best_end<=best_base) gm_panic("no usable memory");
  gm_page_next=best_base;
  gm_page_end=best_end;
  gm_write("memory ");
  gm_print_u64((best_end-best_base)/1024ULL/1024ULL);
  gm_write(" MiB\n");
}

void *gm_phys(uint64_t phys) {
  return (void *)(phys+gm_hhdm);
}

uint64_t gm_page_alloc(void) {
  uint64_t phys;

  if (gm_page_next+GM_PAGE_SIZE>gm_page_end) gm_panic("out of pages");
  phys=gm_page_next;
  gm_page_next+=GM_PAGE_SIZE;
  gm_memset(gm_phys(phys),0,GM_PAGE_SIZE);
  return phys;
}

uint64_t gm_memory_free(void) {
  return gm_page_end-gm_page_next;
}

static uint64_t gm_read_cr3(void) {
  uint64_t cr3;

  __asm__ volatile("mov %%cr3,%0":"=r"(cr3));
  return cr3;
}

static uint64_t *gm_table(uint64_t entry) {
  return (uint64_t *)gm_phys(entry&GM_ADDR_MASK);
}

static uint64_t *gm_next_table(uint64_t *table,uint64_t index,uint64_t flags) {
  uint64_t phys;

  if (!(table[index]&GM_PTE_PRESENT)) {
    phys=gm_page_alloc();
    table[index]=phys|GM_PTE_PRESENT|GM_PTE_WRITE|flags;
  } else {
    table[index]|=flags;
  }
  return gm_table(table[index]);
}

static int gm_map_page_flags(uint64_t cr3,uint64_t virt,uint64_t phys,uint64_t flags) {
  uint64_t parent_flags;
  uint64_t *pml4;
  uint64_t *pdpt;
  uint64_t *pd;
  uint64_t *pt;
  uint64_t index;

  if ((virt&0xfffULL) || (phys&0xfffULL)) return 0;
  parent_flags=flags&GM_PTE_USER;
  pml4=gm_table(cr3);
  pdpt=gm_next_table(pml4,(virt>>39)&0x1ffULL,parent_flags);
  pd=gm_next_table(pdpt,(virt>>30)&0x1ffULL,parent_flags);
  pt=gm_next_table(pd,(virt>>21)&0x1ffULL,parent_flags);
  index=(virt>>12)&0x1ffULL;
  if (pt[index]&GM_PTE_PRESENT) return 0;
  pt[index]=(phys&GM_ADDR_MASK)|flags|GM_PTE_PRESENT;
  if ((gm_read_cr3()&GM_ADDR_MASK)==(cr3&GM_ADDR_MASK))
    __asm__ volatile("invlpg (%0)"::"r"(virt):"memory");
  return 1;
}

uint64_t gm_address_space_current(void) {
  return gm_read_cr3();
}

uint64_t gm_address_space_create(void) {
  uint64_t source_cr3;
  uint64_t phys;
  uint64_t *source;
  uint64_t *target;
  uint64_t i;

  source_cr3=gm_read_cr3();
  phys=gm_page_alloc();
  source=gm_table(source_cr3);
  target=(uint64_t *)gm_phys(phys);
  for (i=256ULL;i<512ULL;i++) target[i]=source[i]&~GM_PTE_USER;
  return phys;
}

void gm_address_space_switch(uint64_t cr3) {
  __asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
}

int gm_map_page(uint64_t virt,uint64_t phys,uint64_t flags) {
  return gm_map_page_flags(gm_read_cr3(),virt,phys,flags);
}

int gm_map_user_page_in(uint64_t cr3,uint64_t virt,uint64_t phys,uint64_t flags) {
  return gm_map_page_flags(cr3,virt,phys,flags|GM_PTE_USER);
}

uint64_t gm_read_cr2(void) {
  uint64_t value;
  __asm__ volatile("mov %%cr2,%0":"=r"(value));
  return value;
}

static void gm_gdt_init(void) {
  struct gm_idt_ptr ptr;
  uint64_t base;
  uint64_t limit;

  gm_memset(gm_gdt,0,sizeof(gm_gdt));
  gm_memset(&gm_tss,0,sizeof(gm_tss));
  gm_gdt[1]=0x00af9a000000ffffULL;
  gm_gdt[2]=0x00cf92000000ffffULL;
  gm_gdt[3]=0x00cff2000000ffffULL;
  gm_gdt[4]=0x00affa000000ffffULL;
  gm_tss.rsp0=(uint64_t)(gm_ring0_stack+sizeof(gm_ring0_stack));
  gm_tss.ist1=(uint64_t)(gm_df_stack+sizeof(gm_df_stack));
  gm_tss.iopb=(uint16_t)sizeof(gm_tss);
  base=(uint64_t)&gm_tss;
  limit=sizeof(gm_tss)-1U;
  gm_gdt[5]=(limit&0xffffULL)|((base&0xffffffULL)<<16)|
            (0x89ULL<<40)|(((limit>>16)&0x0fULL)<<48)|
            (((base>>24)&0xffULL)<<56);
  gm_gdt[6]=base>>32;
  ptr.limit=(uint16_t)(sizeof(gm_gdt)-1U);
  ptr.base=(uint64_t)gm_gdt;
  gm_gdt_load(&ptr);
}

static void gm_idt_gate(uint8_t vector,void (*handler)(void),uint8_t flags,uint8_t ist) {
  uint64_t addr;

  addr=(uint64_t)handler;
  gm_idt[vector].lo=(uint16_t)addr;
  gm_idt[vector].selector=0x08;
  gm_idt[vector].ist=ist;
  gm_idt[vector].flags=flags;
  gm_idt[vector].mid=(uint16_t)(addr>>16);
  gm_idt[vector].hi=(uint32_t)(addr>>32);
  gm_idt[vector].zero=0;
}

void gm_irq_init(void) {
  struct gm_idt_ptr ptr;

  gm_gdt_init();
  gm_memset(gm_idt,0,sizeof(gm_idt));
  gm_idt_gate(0,gm_isr_de,0x8e,0);
  gm_idt_gate(1,gm_isr_db,0x8e,0);
  gm_idt_gate(3,gm_isr_bp,0xef,0);
  gm_idt_gate(4,gm_isr_of,0xef,0);
  gm_idt_gate(5,gm_isr_br,0x8e,0);
  gm_idt_gate(6,gm_isr_ud,0x8e,0);
  gm_idt_gate(7,gm_isr_nm,0x8e,0);
  gm_idt_gate(8,gm_isr_df,0x8e,1);
  gm_idt_gate(10,gm_isr_ts,0x8e,0);
  gm_idt_gate(11,gm_isr_np,0x8e,0);
  gm_idt_gate(12,gm_isr_ss,0x8e,0);
  gm_idt_gate(13,gm_isr_gp,0x8e,0);
  gm_idt_gate(14,gm_isr_pf,0x8e,0);
  gm_idt_gate(16,gm_isr_mf,0x8e,0);
  gm_idt_gate(17,gm_isr_ac,0x8e,0);
  gm_idt_gate(19,gm_isr_xm,0x8e,0);
  gm_idt_gate(32,gm_isr_timer,0x8e,0);
  gm_idt_gate(0x80,gm_isr_service,0xef,0);
  ptr.limit=(uint16_t)(sizeof(gm_idt)-1U);
  ptr.base=(uint64_t)gm_idt;
  __asm__ volatile("lidt %0"::"m"(ptr));

  gm_outb(GM_PIC1_CMD,0x11);
  gm_outb(GM_PIC2_CMD,0x11);
  gm_outb(GM_PIC1_DATA,0x20);
  gm_outb(GM_PIC2_DATA,0x28);
  gm_outb(GM_PIC1_DATA,0x04);
  gm_outb(GM_PIC2_DATA,0x02);
  gm_outb(GM_PIC1_DATA,0x01);
  gm_outb(GM_PIC2_DATA,0x01);
  gm_outb(GM_PIC1_DATA,0xfe);
  gm_outb(GM_PIC2_DATA,0xff);
}

void gm_timer_init(void) {
  uint32_t divisor;

  divisor=GM_PIT_HZ/GM_TICK_HZ;
  gm_tick_count=0;
  gm_outb(GM_PIT_CMD,0x36);
  gm_outb(GM_PIT_CH0,(uint8_t)divisor);
  gm_outb(GM_PIT_CH0,(uint8_t)(divisor>>8));
}

uint64_t gm_ticks(void) {
  return gm_tick_count;
}

void gm_timer_irq(void) {
  gm_tick_count++;
  gm_outb(GM_PIC1_CMD,0x20);
}

void gm_fault_dispatch(uint64_t vector,uint64_t code,uint64_t cs) {
  if (gm_program_fault(vector,code,cs)) return;
  if (vector==14U) {
    gm_write("PF cr2=");
    gm_print_hex(gm_read_cr2());
    gm_write(" code=");
    gm_print_hex(code);
    gm_write("\n");
    gm_panic("page fault");
  }
  if (vector==13U) {
    gm_write("GP code=");
    gm_print_hex(code);
    gm_write("\n");
    gm_panic("general protection");
  }
  gm_write("FAULT vector=");
  gm_print_u64(vector);
  gm_write(" code=");
  gm_print_hex(code);
  gm_write("\n");
  gm_panic("cpu exception");
}
