// Gianluca Mazzini @2026- Version 2.13
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

#define GM_APP_MAX 4U
#define GM_APP_FREE 0U
#define GM_APP_READY 1U
#define GM_APP_RUNNING 2U
#define GM_APP_BLOCKED 3U
#define GM_RESOURCE_NONE 0U
#define GM_RESOURCE_COUNT 2U
#define GM_RESOURCE_BUCKETS 60U
#define GM_RESOURCE_BUCKET_TICKS (10ULL*GM_TICK_HZ)
#define GM_RESOURCE_WINDOW_TICKS (GM_RESOURCE_BUCKETS*GM_RESOURCE_BUCKET_TICKS)
#define GM_PERIODIC_MAX 4U
#define GM_PERIODIC_NAME_MAX 192U
#define GM_PERIODIC_ARGS_MAX 256U
#define GM_START_BUSY 0
#define GM_START_OK 1
#define GM_START_ERROR -1

struct gm_app_slot {
  uint64_t resume_rsp;
  int result;
  uint32_t state;
  uint64_t runtime_ticks;
  uint64_t cr3;
  uint8_t *image;
  uint8_t *arg;
  uint8_t *stack;
  struct gm_service_frame context;
  uint32_t owned_resource;
  uint32_t waiting_resource;
  uint32_t periodic_id;
};

struct gm_resource {
  uint32_t owner;
  uint32_t wait[GM_APP_MAX];
  uint32_t wait_head;
  uint32_t wait_len;
  uint64_t acquired_tick;
  uint64_t total_ticks;
  uint64_t acquisitions;
  uint64_t max_hold;
  uint32_t recent[GM_RESOURCE_BUCKETS];
};

struct gm_periodic {
  uint64_t interval_ticks;
  uint64_t next_tick;
  uint32_t remaining;
  uint32_t slot;
  uint8_t active;
  char name[GM_PERIODIC_NAME_MAX];
  char args[GM_PERIODIC_ARGS_MAX];
};

struct gm_app_slot *gm_program_current;

static struct gm_app_slot gm_apps[GM_APP_MAX];
static struct gm_resource gm_resources[GM_RESOURCE_COUNT];
static struct gm_periodic gm_periodics[GM_PERIODIC_MAX];
static uint64_t gm_resource_epochs[GM_RESOURCE_BUCKETS];
static uint64_t gm_program_kernel_cr3;
static uint32_t gm_program_next;

static void gm_program_kernel(void);

static uint32_t gm_program_slot(const struct gm_app_slot *app) {
  return (uint32_t)(app-gm_apps);
}

static struct gm_resource *gm_resource_get(uint32_t resource) {
  if (!resource || resource>GM_RESOURCE_COUNT) return 0;
  return &gm_resources[resource-1U];
}

static int gm_resource_owned(const struct gm_app_slot *app,uint32_t resource_id) {
  struct gm_resource *resource;
  uint32_t slot;

  resource=gm_resource_get(resource_id);
  if (!resource || app->owned_resource!=resource_id) return 0;
  slot=gm_program_slot(app);
  if (resource->owner!=slot+1U) gm_panic("resource owner");
  return 1;
}

static void gm_resource_bucket(uint64_t epoch) {
  uint32_t bucket;
  uint32_t i;

  bucket=(uint32_t)(epoch%GM_RESOURCE_BUCKETS);
  if (gm_resource_epochs[bucket]==epoch+1ULL) return;
  gm_resource_epochs[bucket]=epoch+1ULL;
  for (i=0;i<GM_RESOURCE_COUNT;i++) gm_resources[i].recent[bucket]=0;
}

static void gm_resource_account(struct gm_resource *resource,uint64_t now) {
  uint64_t start;
  uint64_t end;
  uint64_t epoch;
  uint64_t hold;
  uint32_t bucket;

  start=resource->acquired_tick;
  if (now<start) return;
  hold=now-start;
  resource->total_ticks+=hold;
  if (hold>resource->max_hold) resource->max_hold=hold;
  if (hold>GM_RESOURCE_WINDOW_TICKS) start=now-GM_RESOURCE_WINDOW_TICKS;
  for (;start<now;start=end) {
    epoch=start/GM_RESOURCE_BUCKET_TICKS;
    gm_resource_bucket(epoch);
    bucket=(uint32_t)(epoch%GM_RESOURCE_BUCKETS);
    end=(epoch+1ULL)*GM_RESOURCE_BUCKET_TICKS;
    if (end>now) end=now;
    resource->recent[bucket]+=(uint32_t)(end-start);
  }
}

static int gm_resource_wait(struct gm_resource *resource,uint32_t slot) {
  uint32_t tail;

  if (resource->wait_len>=GM_APP_MAX) return 0;
  tail=(resource->wait_head+resource->wait_len)%GM_APP_MAX;
  resource->wait[tail]=slot;
  resource->wait_len++;
  return 1;
}

static void gm_resource_grant_next(struct gm_resource *resource,uint32_t resource_id,uint64_t now) {
  struct gm_app_slot *app;
  uint32_t slot;

  if (!resource->wait_len) return;
  slot=resource->wait[resource->wait_head];
  resource->wait_head=(resource->wait_head+1U)%GM_APP_MAX;
  resource->wait_len--;
  if (slot>=GM_APP_MAX) gm_panic("resource waiter");
  app=&gm_apps[slot];
  if (app->state!=GM_APP_BLOCKED || app->waiting_resource!=resource_id || app->owned_resource)
    gm_panic("resource wait state");
  resource->owner=slot+1U;
  resource->acquired_tick=now;
  resource->acquisitions++;
  app->waiting_resource=GM_RESOURCE_NONE;
  app->owned_resource=resource_id;
  app->context.rax=1;
  app->state=GM_APP_READY;
}

static int gm_resource_release_app(struct gm_app_slot *app,uint32_t resource_id) {
  struct gm_resource *resource;
  uint32_t slot;
  uint64_t now;

  resource=gm_resource_get(resource_id);
  slot=gm_program_slot(app);
  if (!resource || app->owned_resource!=resource_id || resource->owner!=slot+1U) return 0;
  now=gm_ticks();
  gm_resource_account(resource,now);
  resource->owner=0;
  app->owned_resource=GM_RESOURCE_NONE;
  gm_resource_grant_next(resource,resource_id,now);
  return 1;
}

static void gm_resource_reclaim(struct gm_app_slot *app) {
  uint32_t resource;

  resource=app->owned_resource;
  if (resource && !gm_resource_release_app(app,resource)) gm_panic("resource reclaim");
}

static int gm_resource_acquire_service(struct gm_app_slot *app,uint32_t resource_id,
                                       struct gm_service_frame *frame) {
  struct gm_resource *resource;
  uint32_t slot;

  resource=gm_resource_get(resource_id);
  if (!resource || app->owned_resource || app->waiting_resource) {
    frame->rax=0;
    return 0;
  }
  slot=gm_program_slot(app);
  if (!resource->owner) {
    resource->owner=slot+1U;
    resource->acquired_tick=gm_ticks();
    resource->acquisitions++;
    app->owned_resource=resource_id;
    frame->rax=1;
    return 0;
  }
  if (!gm_resource_wait(resource,slot)) gm_panic("resource wait full");
  gm_memcpy(&app->context,frame,sizeof(app->context));
  app->waiting_resource=resource_id;
  app->state=GM_APP_BLOCKED;
  gm_program_kernel();
  return 1;
}

static const char *gm_app_state_name(uint32_t state) {
  if (state==GM_APP_FREE) return "free";
  if (state==GM_APP_READY) return "ready";
  if (state==GM_APP_RUNNING) return "running";
  if (state==GM_APP_BLOCKED) return "blocked";
  return "invalid";
}

static const char *gm_resource_name(uint32_t resource) {
  if (resource==GM_RESOURCE_TCP) return "tcp";
  if (resource==GM_RESOURCE_UDP) return "udp";
  return "-";
}

static uint64_t gm_resource_live(const struct gm_resource *resource,uint64_t now) {
  if (!resource->owner || now<resource->acquired_tick) return 0;
  return now-resource->acquired_tick;
}

static uint64_t gm_resource_recent(const struct gm_resource *resource,uint64_t now) {
  uint64_t current_epoch;
  uint64_t epoch;
  uint64_t start;
  uint64_t recent;
  uint32_t i;

  current_epoch=now/GM_RESOURCE_BUCKET_TICKS;
  recent=0;
  for (i=0;i<GM_RESOURCE_BUCKETS;i++) {
    if (!gm_resource_epochs[i]) continue;
    epoch=gm_resource_epochs[i]-1ULL;
    if (epoch<=current_epoch && current_epoch-epoch<GM_RESOURCE_BUCKETS) recent+=resource->recent[i];
  }
  if (resource->owner && now>=resource->acquired_tick) {
    start=resource->acquired_tick;
    if (now>GM_RESOURCE_WINDOW_TICKS && start<now-GM_RESOURCE_WINDOW_TICKS)
      start=now-GM_RESOURCE_WINDOW_TICKS;
    recent+=now-start;
  }
  return recent;
}

void gm_programs_status(void) {
  struct gm_app_slot *app;
  uint32_t i;

  for (i=0;i<GM_APP_MAX;i++) {
    app=&gm_apps[i];
    gm_write("app ");
    gm_print_u64(i);
    gm_write(" ");
    gm_write(gm_app_state_name(app->state));
    gm_write(" ticks=");
    gm_print_u64(app->runtime_ticks);
    gm_write(" owns=");
    gm_write(gm_resource_name(app->owned_resource));
    gm_write(" waits=");
    gm_write(gm_resource_name(app->waiting_resource));
    gm_write("\n");
  }
}

static void gm_resource_status(uint32_t resource_id,uint64_t now) {
  struct gm_resource *resource;
  uint64_t live;
  uint64_t total;
  uint64_t longest;

  resource=gm_resource_get(resource_id);
  if (!resource) return;
  live=gm_resource_live(resource,now);
  total=resource->total_ticks+live;
  longest=resource->max_hold;
  if (live>longest) longest=live;
  gm_write(gm_resource_name(resource_id));
  gm_write(" owner=");
  if (resource->owner) {
    gm_write("app");
    gm_print_u64(resource->owner-1U);
  } else gm_write("free");
  gm_write(" held=");
  gm_print_u64(live);
  gm_write(" waiters=");
  gm_print_u64(resource->wait_len);
  gm_write(" acquisitions=");
  gm_print_u64(resource->acquisitions);
  gm_write(" total=");
  gm_print_u64(total);
  gm_write(" max=");
  gm_print_u64(longest);
  gm_write(" recent10m=");
  gm_print_u64(gm_resource_recent(resource,now));
  gm_write(" ticks\n");
}

void gm_resources_status(void) {
  uint64_t now;
  uint32_t i;

  now=gm_ticks();
  for (i=1U;i<=GM_RESOURCE_COUNT;i++) gm_resource_status(i,now);
}

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
  gm_memset(gm_program_current->image,0,GM_PROGRAM_IMAGE_MAX);
  gm_memset(gm_program_current->arg,0,GM_PROGRAM_ARG_SIZE);
  gm_memset(gm_program_current->stack,0,GM_PROGRAM_STACK_SIZE);
}

static void gm_program_activate(struct gm_app_slot *app) {
  gm_program_current=app;
  gm_address_space_switch(app->cr3);
}

static void gm_program_kernel(void) {
  gm_address_space_switch(gm_program_kernel_cr3);
}

static void gm_program_isolation_check(void) {
  volatile uint64_t *probe;

  probe=(volatile uint64_t *)GM_PROGRAM_BASE;
  gm_program_activate(&gm_apps[0]);
  *probe=0x1122334455667788ULL;
  gm_program_activate(&gm_apps[1]);
  if (*probe) gm_panic("program isolation");
  *probe=0x8877665544332211ULL;
  gm_program_activate(&gm_apps[0]);
  if (*probe!=0x1122334455667788ULL) gm_panic("program isolation");
  gm_program_activate(&gm_apps[1]);
  if (*probe!=0x8877665544332211ULL) gm_panic("program isolation");
  gm_program_clear();
  gm_program_activate(&gm_apps[0]);
  gm_program_clear();
  gm_program_kernel();
  gm_program_current=&gm_apps[0];
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
    if (!gm_store_read(path,GM_PROGRAM_HEADER_SIZE+offset,gm_program_current->image+offset,want,&got) || got!=want) return 0;
    offset+=want;
  }
  if (gm_image_checksum(gm_program_current->image,payload)!=header->checksum) return 0;
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
  struct gm_app_slot *app;
  uint64_t phys;
  uint64_t virt;
  uint64_t i;

  gm_program_kernel_cr3=gm_address_space_current();
  gm_memset(gm_apps,0,sizeof(gm_apps));
  gm_memset(gm_resources,0,sizeof(gm_resources));
  gm_memset(gm_periodics,0,sizeof(gm_periodics));
  gm_memset(gm_resource_epochs,0,sizeof(gm_resource_epochs));
  for (i=0;i<GM_APP_MAX;i++) {
    app=&gm_apps[i];
    app->cr3=gm_address_space_create();
    app->image=(uint8_t *)GM_PROGRAM_BASE;
    app->arg=(uint8_t *)GM_PROGRAM_ARG;
    app->stack=(uint8_t *)GM_PROGRAM_STACK;
    for (virt=GM_PROGRAM_BASE;virt<GM_PROGRAM_END;virt+=GM_PAGE_SIZE) {
      if (virt==GM_PROGRAM_GUARD) continue;
      phys=gm_page_alloc();
      if (!gm_map_user_page_in(app->cr3,virt,phys,0x002ULL)) gm_panic("program map");
    }
    gm_program_activate(app);
    gm_program_clear();
    app->state=GM_APP_FREE;
    app->result=0;
    app->runtime_ticks=0;
    app->resume_rsp=0;
    app->owned_resource=GM_RESOURCE_NONE;
    app->waiting_resource=GM_RESOURCE_NONE;
    app->periodic_id=0;
    gm_memset(&app->context,0,sizeof(app->context));
    gm_program_kernel();
  }
  gm_program_current=&gm_apps[0];
  gm_program_next=0;
  gm_program_isolation_check();
}

void gm_programs_list(void) {
  if (!gm_tcp_connected() && !gm_store_connect()) {
    gm_write("programs unavailable\n");
    return;
  }
  if (!gm_store_list_prefix("/programs/" GM_ARCH_NAME "/")) gm_write("programs unavailable\n");
}

static int gm_program_start_path(const char *path,const char *args,uint32_t periodic_id,uint32_t *slot_out,int report) {
  struct gm_app_slot *app;
  struct gm_image_header header;
  uint64_t arg_len;
  uint64_t stack;
  uint32_t slot;

  app=0;
  slot=0;
  for (;slot<GM_APP_MAX;slot++) {
    if (gm_apps[slot].state==GM_APP_FREE) {
      app=&gm_apps[slot];
      break;
    }
  }
  if (!app) {
    if (report) gm_write("no free app slot\n");
    return GM_START_BUSY;
  }
  gm_program_activate(app);
  if (!gm_program_load(path,&header)) {
    gm_program_kernel();
    if (report) gm_write("program load error\n");
    return GM_START_ERROR;
  }
  arg_len=args?gm_strlen(args):0;
  if (arg_len>=GM_PROGRAM_ARG_SIZE-16U) {
    gm_program_kernel();
    if (report) gm_write("program args too long\n");
    return GM_START_ERROR;
  }
  if (arg_len) gm_memcpy(app->arg,args,arg_len);
  app->arg[arg_len]=0;
  stack=GM_PROGRAM_STACK+GM_PROGRAM_STACK_SIZE-8U;
  gm_program_return_stub((uint64_t *)stack);
  gm_memset(&app->context,0,sizeof(app->context));
  app->context.rdi=GM_PROGRAM_ARG;
  app->context.rsi=arg_len;
  app->context.rip=GM_PROGRAM_BASE+header.entry;
  app->context.cs=0x23ULL;
  app->context.rflags=0x202ULL;
  app->context.rsp=stack;
  app->context.ss=0x1bULL;
  app->runtime_ticks=0;
  app->result=-1;
  app->owned_resource=GM_RESOURCE_NONE;
  app->waiting_resource=GM_RESOURCE_NONE;
  app->periodic_id=periodic_id;
  app->state=GM_APP_READY;
  gm_program_kernel();
  if (slot_out) *slot_out=slot;
  gm_write("started slot ");
  gm_print_u64(slot);
  gm_write(" ");
  gm_write(path);
  gm_write("\n");
  return GM_START_OK;
}

int gm_program_run(const char *name,const char *args) {
  char path[GM_PERIODIC_NAME_MAX];

  if (!gm_program_path(name,path)) return 0;
  (void)gm_program_start_path(path,args,0,0,1);
  return 1;
}

int gm_periodic_add(uint64_t seconds,uint32_t count,const char *name,const char *args) {
  struct gm_periodic *periodic;
  char path[GM_PERIODIC_NAME_MAX];
  uint64_t arg_len;
  uint64_t interval;
  uint64_t now;
  uint32_t i;

  if (!seconds || !count || seconds>~0ULL/GM_TICK_HZ || !gm_program_path(name,path)) return 0;
  arg_len=args?gm_strlen(args):0;
  if (arg_len>=GM_PERIODIC_ARGS_MAX) return 0;
  periodic=0;
  for (i=0;i<GM_PERIODIC_MAX;i++) {
    if (!gm_periodics[i].active) {
      periodic=&gm_periodics[i];
      break;
    }
  }
  if (!periodic) return 0;
  interval=seconds*GM_TICK_HZ;
  now=gm_ticks();
  if (now>~0ULL-interval) return 0;
  gm_memset(periodic,0,sizeof(*periodic));
  gm_memcpy(periodic->name,path,gm_strlen(path)+1U);
  if (arg_len) gm_memcpy(periodic->args,args,arg_len);
  periodic->args[arg_len]=0;
  periodic->interval_ticks=interval;
  periodic->next_tick=now+interval;
  periodic->remaining=count;
  periodic->slot=GM_APP_MAX;
  periodic->active=1;
  gm_write("periodic ");
  gm_print_u64(i);
  gm_write(" scheduled every=");
  gm_print_u64(seconds);
  gm_write("s count=");
  gm_print_u64(count);
  gm_write(" app=");
  gm_write(path);
  gm_write("\n");
  return 1;
}

int gm_periodic_cancel(uint32_t id) {
  if (id>=GM_PERIODIC_MAX || !gm_periodics[id].active) return 0;
  gm_periodics[id].active=0;
  gm_write("periodic ");
  gm_print_u64(id);
  gm_write(" cancelled\n");
  return 1;
}

void gm_periodics_status(void) {
  struct gm_periodic *periodic;
  uint64_t now;
  uint64_t due;
  uint32_t i;
  int found;

  now=gm_ticks();
  found=0;
  for (i=0;i<GM_PERIODIC_MAX;i++) {
    periodic=&gm_periodics[i];
    if (!periodic->active) continue;
    found=1;
    due=periodic->next_tick>now?(periodic->next_tick-now+GM_TICK_HZ-1U)/GM_TICK_HZ:0;
    gm_write("periodic ");
    gm_print_u64(i);
    gm_write(" every=");
    gm_print_u64(periodic->interval_ticks/GM_TICK_HZ);
    gm_write("s remaining=");
    gm_print_u64(periodic->remaining);
    gm_write(" due=");
    gm_print_u64(due);
    gm_write("s running=");
    if (periodic->slot<GM_APP_MAX) {
      gm_write("app");
      gm_print_u64(periodic->slot);
    } else gm_write("-");
    gm_write(" app=");
    gm_write(periodic->name);
    if (periodic->args[0]) {
      gm_write(" args=");
      gm_write(periodic->args);
    }
    gm_write("\n");
  }
  if (!found) gm_write("no periodics\n");
}

void gm_periodic_poll(void) {
  struct gm_periodic *periodic;
  uint64_t now;
  uint32_t slot;
  uint32_t i;
  int rc;

  now=gm_ticks();
  for (i=0;i<GM_PERIODIC_MAX;i++) {
    periodic=&gm_periodics[i];
    if (!periodic->active) continue;
    if (periodic->slot<GM_APP_MAX) {
      if (gm_apps[periodic->slot].periodic_id==i+1U && gm_apps[periodic->slot].state!=GM_APP_FREE) continue;
      periodic->slot=GM_APP_MAX;
      if (!periodic->remaining) {
        periodic->active=0;
        gm_write("periodic ");
        gm_print_u64(i);
        gm_write(" complete\n");
        continue;
      }
    }
    if (now<periodic->next_tick) continue;
    slot=GM_APP_MAX;
    rc=gm_program_start_path(periodic->name,periodic->args,i+1U,&slot,0);
    if (rc==GM_START_BUSY) continue;
    if (rc==GM_START_ERROR) {
      periodic->next_tick=now+periodic->interval_ticks;
      gm_write("periodic ");
      gm_print_u64(i);
      gm_write(" launch error\n");
      continue;
    }
    periodic->slot=slot;
    periodic->remaining--;
    periodic->next_tick=now+periodic->interval_ticks;
  }
}

static void gm_program_result(struct gm_app_slot *app) {
  gm_write("app ");
  gm_print_u64(gm_program_slot(app));
  gm_write(" returned ");
  if (app->result<0) {
    gm_write("-");
    gm_print_u64((uint64_t)(-app->result));
  } else {
    gm_print_u64((uint64_t)app->result);
  }
  gm_write("\n");
}

static void gm_program_finish(struct gm_app_slot *app,int result) {
  app->result=result;
  gm_resource_reclaim(app);
  app->state=GM_APP_FREE;
  gm_program_kernel();
  gm_program_result(app);
}

int gm_program_schedule(void) {
  struct gm_app_slot *app;
  uint32_t i;
  uint32_t slot;

  for (i=0;i<GM_APP_MAX;i++) {
    slot=(gm_program_next+i)%GM_APP_MAX;
    app=&gm_apps[slot];
    if (app->state!=GM_APP_READY) continue;
    gm_program_next=(slot+1U)%GM_APP_MAX;
    gm_program_activate(app);
    app->state=GM_APP_RUNNING;
    gm_program_resume_user(&app->context);
    return 1;
  }
  return 0;
}

static int gm_program_yield(struct gm_service_frame *frame) {
  struct gm_app_slot *app;

  app=gm_program_current;
  gm_memcpy(&app->context,frame,sizeof(app->context));
  app->state=GM_APP_READY;
  gm_program_kernel();
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
  if (gm_program_current->state!=GM_APP_RUNNING || (frame->cs&3U)!=3U) return 0;
  if (gm_program_current->runtime_ticks>=GM_PROGRAM_MAX_TICKS) {
    gm_write("program timeout\n");
    gm_program_finish(gm_program_current,-200);
    return 1;
  }
  if (frame->rax==GM_SVC_EXIT) {
    gm_program_finish(gm_program_current,(int)(int32_t)frame->rdi);
    return 1;
  }
  if (frame->rax==GM_SVC_WRITE) {
    if (!gm_user_string(frame->rdi,1024U,&len)) frame->rax=0;
    else {
      gm_write((const char *)frame->rdi);
      frame->rax=1;
    }
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_PRINT_U64) {
    gm_print_u64(frame->rdi);
    frame->rax=1;
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_TICKS) {
    frame->rax=gm_ticks();
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_GATEWAY) {
    if (!gm_user_range(frame->rdi,4)) frame->rax=0;
    else {
      gm_copy_ip(gateway,gm_config.gateway);
      gm_memcpy((void *)frame->rdi,gateway,4);
      frame->rax=1;
    }
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_PING) {
    if (!gm_user_range(frame->rdi,4)) frame->rax=0;
    else frame->rax=gm_ping((const uint8_t *)frame->rdi,frame->rsi>500U?500U:frame->rsi);
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_UDP_EXCHANGE) {
    if (!gm_resource_owned(gm_program_current,GM_RESOURCE_UDP) || !frame->rsi || frame->rsi>65535U ||
        frame->rcx>GM_UDP_MAX || frame->r9>GM_UDP_MAX || !gm_user_range(frame->rdi,4) ||
        (frame->rcx && !gm_user_range(frame->rdx,frame->rcx)) ||
        (frame->r9 && !gm_user_range(frame->r8,frame->r9))) frame->rax=0;
    else frame->rax=gm_udp_exchange((const uint8_t *)frame->rdi,(uint16_t)frame->rsi,
                                    (const uint8_t *)frame->rdx,(uint16_t)frame->rcx,
                                    (uint8_t *)frame->r8,(uint16_t)frame->r9,300U);
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_RESOURCE_ACQUIRE) {
    if (gm_resource_acquire_service(gm_program_current,(uint32_t)frame->rdi,frame)) return 1;
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_RESOURCE_RELEASE) {
    frame->rax=gm_resource_release_app(gm_program_current,(uint32_t)frame->rdi);
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_STORE_STAT) {
    if (!gm_resource_owned(gm_program_current,GM_RESOURCE_TCP)) {
      frame->rax=0;
      return gm_program_yield(frame);
    }
    if (!gm_user_string(frame->rdi,191U,0) || !gm_user_range(frame->rsi,sizeof(uint64_t))) {
      frame->rax=0;
      return gm_program_yield(frame);
    }
    if (!gm_tcp_connected() && !gm_store_connect()) {
      frame->rax=0;
      return gm_program_yield(frame);
    }
    len=0;
    ok=gm_store_size((const char *)frame->rdi,&len);
    if (ok) gm_memcpy((void *)frame->rsi,&len,sizeof(len));
    frame->rax=ok;
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_STORE_READ) {
    if (!gm_resource_owned(gm_program_current,GM_RESOURCE_TCP)) {
      frame->rax=0;
      return gm_program_yield(frame);
    }
    if (!gm_tcp_connected() && !gm_store_connect()) {
      frame->rax=0;
      return gm_program_yield(frame);
    }
    if (!gm_user_string(frame->rdi,191U,0) || frame->rcx>GM_STORE_BLOCK ||
        !gm_user_range(frame->rdx,frame->rcx)) frame->rax=0;
    else {
      size=(uint16_t)frame->rcx;
      got=0;
      ok=gm_store_read((const char *)frame->rdi,frame->rsi,(uint8_t *)frame->rdx,size,&got);
      frame->rax=ok?got:0;
    }
    return gm_program_yield(frame);
  }
  if (frame->rax==GM_SVC_STORE_WRITE || frame->rax==GM_SVC_STORE_APPEND) {
    if (!gm_resource_owned(gm_program_current,GM_RESOURCE_TCP)) {
      frame->rax=0;
      return gm_program_yield(frame);
    }
    if (!gm_tcp_connected() && !gm_store_connect()) {
      frame->rax=0;
      return gm_program_yield(frame);
    }
    if (!gm_user_string(frame->rdi,191U,0) || frame->rdx>GM_STORE_BLOCK ||
        !gm_user_range(frame->rsi,frame->rdx)) frame->rax=0;
    else frame->rax=gm_store_write((const char *)frame->rdi,(const uint8_t *)frame->rsi,
                                   (uint16_t)frame->rdx,frame->rax==GM_SVC_STORE_APPEND);
    return gm_program_yield(frame);
  }
  frame->rax=0;
  return gm_program_yield(frame);
}

int gm_program_fault(uint64_t vector,uint64_t code,uint64_t cs) {
  struct gm_app_slot *app;
  int result;

  app=gm_program_current;
  if (app->state!=GM_APP_RUNNING || (cs&3U)!=3U) return 0;
  result=-100-(int)vector;
  gm_program_kernel();
  gm_write("program fault vector=");
  gm_print_u64(vector);
  gm_write(" code=");
  gm_print_hex(code);
  gm_write("\n");
  app->result=result;
  gm_resource_reclaim(app);
  app->state=GM_APP_FREE;
  gm_program_result(app);
  return 1;
}

int gm_program_preempt(void *raw) {
  struct gm_service_frame *frame;
  struct gm_app_slot *app;

  frame=(struct gm_service_frame *)raw;
  app=gm_program_current;
  if (app->state!=GM_APP_RUNNING || (frame->cs&3U)!=3U) return 0;
  app->runtime_ticks++;
  if (app->runtime_ticks>=GM_PROGRAM_MAX_TICKS) {
    gm_program_kernel();
    gm_write("program timeout\n");
    app->result=-200;
    gm_resource_reclaim(app);
    app->state=GM_APP_FREE;
    gm_program_result(app);
    return 1;
  }
  gm_memcpy(&app->context,frame,sizeof(app->context));
  app->state=GM_APP_READY;
  gm_program_kernel();
  return 1;
}
