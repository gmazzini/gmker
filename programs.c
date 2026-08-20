// Gianluca Mazzini @2026- Version 2.01
#include "gmker.h"


struct gm_image_header {
  uint32_t magic;
  uint32_t api;
  uint32_t arch;
  uint32_t image_size;
  uint32_t entry;
  uint32_t checksum;
} __attribute__((packed));

struct gm_service_frame {
  uint64_t rax;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t rbp;
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
};

volatile uint64_t gm_program_resume_rsp;
volatile int gm_program_result;

static uint8_t *gm_program_image;
static uint8_t *gm_program_arg;
static uint8_t *gm_program_stack;
static uint64_t gm_program_started;
static int gm_program_running;

static uint32_t gm_u32(const uint8_t *p) {
  return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

static uint32_t gm_image_checksum(const uint8_t *data,uint32_t len) {
  uint32_t hash;
  uint32_t i;

  hash=2166136261U;
  for (i=0;i<len;i++) {
    hash^=data[i];
    hash*=16777619U;
  }
  return hash;
}

static int gm_user_range(uint64_t addr,uint64_t len) {
  uint64_t end;

  if (addr<GM_PROGRAM_BASE || addr>=GM_PROGRAM_END) return 0;
  if (len>GM_PROGRAM_END-GM_PROGRAM_BASE) return 0;
  end=addr+len;
  if (end<addr || end>GM_PROGRAM_END) return 0;
  if (addr<GM_PROGRAM_GUARD && end<=GM_PROGRAM_GUARD) return 1;
  if (addr>=GM_PROGRAM_STACK && end<=GM_PROGRAM_END) return 1;
  return 0;
}

static int gm_user_string(uint64_t addr,uint64_t max,uint64_t *len) {
  const char *s;
  uint64_t i;

  if (!gm_user_range(addr,1)) return 0;
  s=(const char *)addr;
  for (i=0;i<max;i++) {
    if (!gm_user_range(addr+i,1)) return 0;
    if (!s[i]) {
      if (len) *len=i;
      return 1;
    }
  }
  return 0;
}

static void gm_program_clear(void) {
  gm_memset(gm_program_image,0,GM_PROGRAM_IMAGE_MAX);
  gm_memset(gm_program_arg,0,GM_PROGRAM_ARG_SIZE);
  gm_memset(gm_program_stack,0,GM_PROGRAM_STACK_SIZE);
}

static int gm_program_path(const char *name,char path[192]) {
  static const char prefix[]="/programs/" GM_ARCH_NAME "/";
  uint64_t prefix_len;
  uint64_t len;

  if (!name || !*name) return 0;
  prefix_len=sizeof(prefix)-1U;
  if (name[0]=='/') {
    len=gm_strlen(name);
    if (len>=192U || !gm_starts(name,prefix)) return 0;
    gm_memcpy(path,name,len+1U);
    return 1;
  }
  len=gm_strlen(name);
  if (!len || prefix_len+len+4U>=192U) return 0;
  gm_memcpy(path,prefix,prefix_len);
  gm_memcpy(path+prefix_len,name,len);
  if (len<3U || !gm_streq(name+len-3U,".gm")) gm_memcpy(path+prefix_len+len,".gm",4);
  else path[prefix_len+len]=0;
  return 1;
}
static int gm_program_load(const char *path,struct gm_image_header *header) {
  uint8_t raw[GM_PROGRAM_HEADER_SIZE];
  uint64_t file_size;
  uint64_t offset;
  uint64_t remain;
  uint32_t payload;
  uint16_t got;
  uint16_t want;

  if (!gm_tcp_connected() && !gm_store_connect()) return 0;
  if (!gm_store_size(path,&file_size)) return 0;
  if (file_size<GM_PROGRAM_HEADER_SIZE || file_size>GM_PROGRAM_HEADER_SIZE+GM_PROGRAM_IMAGE_MAX) return 0;
  if (!gm_store_read(path,0,raw,sizeof(raw),&got) || got!=sizeof(raw)) return 0;
  header->magic=gm_u32(raw);
  header->api=gm_u32(raw+4);
  header->arch=gm_u32(raw+8);
  header->image_size=gm_u32(raw+12);
  header->entry=gm_u32(raw+16);
  header->checksum=gm_u32(raw+20);
  if (header->magic!=GM_IMAGE_MAGIC || header->api!=GM_API_VERSION || header->arch!=GM_ARCH_NATIVE) return 0;
  payload=header->image_size;
  if (!payload || payload>GM_PROGRAM_IMAGE_MAX || file_size!=GM_PROGRAM_HEADER_SIZE+(uint64_t)payload) return 0;
  if (header->entry>=payload) return 0;
  gm_program_clear();
  offset=0;
  remain=payload;
  for (;remain;remain-=want) {
    want=remain>GM_STORE_BLOCK?GM_STORE_BLOCK:(uint16_t)remain;
    if (!gm_store_read(path,GM_PROGRAM_HEADER_SIZE+offset,gm_program_image+offset,want,&got) || got!=want) return 0;
    offset+=want;
  }
  if (gm_image_checksum(gm_program_image,payload)!=header->checksum) return 0;
  return 1;
}

static void gm_program_return_stub(uint64_t *stack) {
  static const uint8_t code[]={0x89,0xc7,0xb8,0x00,0x00,0x00,0x00,0xcd,0x80,0xf4,0xeb,0xfd};
  uint64_t addr;

  addr=GM_PROGRAM_ARG+GM_PROGRAM_ARG_SIZE-sizeof(code);
  gm_memcpy((void *)addr,code,sizeof(code));
  *stack=addr;
}

void gm_programs_init(void) {
  uint64_t phys;
  uint64_t virt;
  uint64_t i;

  gm_program_image=(uint8_t *)GM_PROGRAM_BASE;
  gm_program_arg=(uint8_t *)GM_PROGRAM_ARG;
  gm_program_stack=(uint8_t *)GM_PROGRAM_STACK;
  for (virt=GM_PROGRAM_BASE;virt<GM_PROGRAM_END;virt+=GM_PAGE_SIZE) {
    if (virt==GM_PROGRAM_GUARD) continue;
    phys=gm_page_alloc();
    if (!gm_map_user_page(virt,phys,0x002ULL)) gm_panic("program map");
  }
  for (i=0;i<GM_PROGRAM_END-GM_PROGRAM_BASE;i+=GM_PAGE_SIZE) {
    if (GM_PROGRAM_BASE+i==GM_PROGRAM_GUARD) continue;
    gm_memset((void *)(GM_PROGRAM_BASE+i),0,GM_PAGE_SIZE);
  }
  gm_program_running=0;
  gm_program_result=0;
  gm_program_resume_rsp=0;
}

void gm_programs_list(void) {
  if (!gm_tcp_connected() && !gm_store_connect()) {
    gm_write("programs unavailable\n");
    return;
  }
  if (!gm_store_list_prefix("/programs/" GM_ARCH_NAME "/")) gm_write("programs unavailable\n");
}

int gm_program_run(const char *name,const char *args) {
  struct gm_image_header header;
  char path[192];
  uint64_t arg_len;
  uint64_t stack;
  int result;

  if (gm_program_running || !gm_program_path(name,path)) return 0;
  if (!gm_program_load(path,&header)) {
    gm_write("program load error\n");
    return 1;
  }
  arg_len=gm_strlen(args);
  if (arg_len>=GM_PROGRAM_ARG_SIZE-16U) {
    gm_write("program args too long\n");
    return 1;
  }
  if (args && arg_len) gm_memcpy(gm_program_arg,args,arg_len);
  gm_program_arg[arg_len]=0;
  stack=GM_PROGRAM_STACK+GM_PROGRAM_STACK_SIZE-8U;
  gm_program_return_stub((uint64_t *)stack);
  gm_program_running=1;
  gm_program_started=gm_ticks();
  gm_program_result=-1;
  gm_write("program ");
  gm_write(path);
  gm_write("\n");
  result=gm_program_enter(GM_PROGRAM_BASE+header.entry,stack,GM_PROGRAM_ARG,arg_len);
  gm_program_running=0;
  gm_write("returned ");
  if (result<0) {
    gm_write("-");
    gm_print_u64((uint64_t)(-result));
  } else {
    gm_print_u64((uint64_t)result);
  }
  gm_write("\n");
  return 1;
}

int gm_program_service(void *raw) {
  struct gm_service_frame *frame;
  uint64_t len;
  uint16_t got;
  uint16_t size;
  uint8_t gateway[4];
  int ok;

  frame=(struct gm_service_frame *)raw;
  if (!gm_program_running || (frame->cs&3U)!=3U) return 0;
  if (gm_ticks()-gm_program_started>=GM_PROGRAM_MAX_TICKS) {
    gm_program_result=-200;
    gm_write("program timeout\n");
    return 1;
  }
  if (frame->rax==GM_SVC_EXIT) {
    gm_program_result=(int)(int32_t)frame->rdi;
    return 1;
  }
  if (frame->rax==GM_SVC_WRITE) {
    if (!gm_user_string(frame->rdi,1024U,&len)) frame->rax=0;
    else {
      gm_write((const char *)frame->rdi);
      frame->rax=1;
    }
    return 0;
  }
  if (frame->rax==GM_SVC_PRINT_U64) {
    gm_print_u64(frame->rdi);
    frame->rax=1;
    return 0;
  }
  if (frame->rax==GM_SVC_TICKS) {
    frame->rax=gm_ticks();
    return 0;
  }
  if (frame->rax==GM_SVC_GATEWAY) {
    if (!gm_user_range(frame->rdi,4)) frame->rax=0;
    else {
      gm_copy_ip(gateway,gm_config.gateway);
      gm_memcpy((void *)frame->rdi,gateway,4);
      frame->rax=1;
    }
    return 0;
  }
  if (frame->rax==GM_SVC_PING) {
    if (!gm_user_range(frame->rdi,4)) frame->rax=0;
    else frame->rax=gm_ping((const uint8_t *)frame->rdi,frame->rsi>500U?500U:frame->rsi);
    return 0;
  }
  if (frame->rax==GM_SVC_STORE_READ) {
    if (!gm_tcp_connected() && !gm_store_connect()) {
      frame->rax=0;
      return 0;
    }
    if (!gm_user_string(frame->rdi,191U,0) || frame->rcx>GM_STORE_BLOCK ||
        !gm_user_range(frame->rdx,frame->rcx)) frame->rax=0;
    else {
      size=(uint16_t)frame->rcx;
      got=0;
      ok=gm_store_read((const char *)frame->rdi,frame->rsi,(uint8_t *)frame->rdx,size,&got);
      frame->rax=ok?got:0;
    }
    return 0;
  }
  if (frame->rax==GM_SVC_STORE_WRITE || frame->rax==GM_SVC_STORE_APPEND) {
    if (!gm_tcp_connected() && !gm_store_connect()) {
      frame->rax=0;
      return 0;
    }
    if (!gm_user_string(frame->rdi,191U,0) || frame->rdx>GM_STORE_BLOCK ||
        !gm_user_range(frame->rsi,frame->rdx)) frame->rax=0;
    else frame->rax=gm_store_write((const char *)frame->rdi,(const uint8_t *)frame->rsi,
                                   (uint16_t)frame->rdx,frame->rax==GM_SVC_STORE_APPEND);
    return 0;
  }
  frame->rax=0;
  return 0;
}

int gm_program_fault(uint64_t vector,uint64_t code,uint64_t cs) {
  if (!gm_program_running || (cs&3U)!=3U) return 0;
  gm_program_result=-100-(int)vector;
  gm_write("program fault vector=");
  gm_print_u64(vector);
  gm_write(" code=");
  gm_print_hex(code);
  gm_write("\n");
  return 1;
}

int gm_program_tick(uint64_t cs) {
  if (!gm_program_running || (cs&3U)!=3U) return 0;
  if (gm_ticks()-gm_program_started<GM_PROGRAM_MAX_TICKS) return 0;
  gm_program_result=-200;
  gm_write("program timeout\n");
  return 1;
}
