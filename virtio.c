// Gianluca Mazzini @2026- Version 2.02
#include "gmker.h"

#define GM_PCI_ADDR 0xcf8
#define GM_PCI_DATA 0xcfc
#define GM_VIRTIO_VENDOR 0x1af4
#define GM_VIRTIO_NET_DEVICE 0x1041
#define GM_VIRTIO_CAP_ID 0x09
#define GM_VIRTIO_CAP_COMMON 1
#define GM_VIRTIO_CAP_NOTIFY 2
#define GM_VIRTIO_CAP_DEVICE 4
#define GM_VIRTIO_MAP_COMMON 0xffff900000100000ULL
#define GM_VIRTIO_MAP_NOTIFY 0xffff900000110000ULL
#define GM_VIRTIO_MAP_DEVICE 0xffff900000120000ULL
#define GM_VIRTIO_MAP_MAX 0x10000ULL
#define GM_VIRTIO_QUEUE_SIZE 8U
#define GM_VIRTIO_RX_QUEUE 0U
#define GM_VIRTIO_TX_QUEUE 1U
#define GM_VIRTIO_NET_HDR 12U
#define GM_VIRTIO_BUF_SIZE 2048U
#define GM_VIRTIO_STATUS_ACK 1U
#define GM_VIRTIO_STATUS_DRIVER 2U
#define GM_VIRTIO_STATUS_DRIVER_OK 4U
#define GM_VIRTIO_STATUS_FEATURES_OK 8U
#define GM_VIRTIO_STATUS_NEEDS_RESET 64U
#define GM_VIRTIO_STATUS_FAILED 128U
#define GM_VIRTIO_F_VERSION_1 0x00000001U
#define GM_VIRTIO_NET_F_MAC (1U<<5)
#define GM_VIRTQ_DESC_F_WRITE 2U
#define GM_VIRTQ_AVAIL_F_NO_INTERRUPT 1U
#define GM_VIRTQ_DESC_OFF 0U
#define GM_VIRTQ_AVAIL_OFF 128U
#define GM_VIRTQ_USED_OFF 160U
#define GM_VIRTQ_NO_VECTOR 0xffffU
#define GM_MMIO_FLAGS 0x012ULL

struct gm_virtio_common {
  volatile uint32_t device_feature_select;
  volatile uint32_t device_feature;
  volatile uint32_t driver_feature_select;
  volatile uint32_t driver_feature;
  volatile uint16_t msix_config;
  volatile uint16_t num_queues;
  volatile uint8_t device_status;
  volatile uint8_t config_generation;
  volatile uint16_t queue_select;
  volatile uint16_t queue_size;
  volatile uint16_t queue_msix_vector;
  volatile uint16_t queue_enable;
  volatile uint16_t queue_notify_off;
  volatile uint64_t queue_desc;
  volatile uint64_t queue_driver;
  volatile uint64_t queue_device;
} __attribute__((packed));

struct gm_virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed));

struct gm_virtq_avail {
  volatile uint16_t flags;
  volatile uint16_t idx;
  volatile uint16_t ring[GM_VIRTIO_QUEUE_SIZE];
} __attribute__((packed));

struct gm_virtq_used_elem {
  volatile uint32_t id;
  volatile uint32_t len;
} __attribute__((packed));

struct gm_virtq_used {
  volatile uint16_t flags;
  volatile uint16_t idx;
  struct gm_virtq_used_elem ring[GM_VIRTIO_QUEUE_SIZE];
} __attribute__((packed));

struct gm_virtq {
  struct gm_virtq_desc *desc;
  struct gm_virtq_avail *avail;
  struct gm_virtq_used *used;
  volatile uint16_t *notify;
  uint16_t last_used;
};

struct gm_virtio_cap {
  uint8_t bar;
  uint32_t offset;
  uint32_t length;
  uint32_t multiplier;
  int found;
};

static volatile struct gm_virtio_common *gm_vcommon;
static volatile uint8_t *gm_vdevice;
static struct gm_virtq gm_vrx;
static struct gm_virtq gm_vtx;
static uint8_t *gm_rx_buf[GM_VIRTIO_QUEUE_SIZE];
static uint8_t *gm_tx_buf[GM_VIRTIO_QUEUE_SIZE];
static uint8_t gm_tx_free[GM_VIRTIO_QUEUE_SIZE];
static uint16_t gm_tx_next;
static int gm_virtio_ready;

static void gm_barrier(void) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void gm_acquire(void) {
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static void gm_release(void) {
  __atomic_thread_fence(__ATOMIC_RELEASE);
}

static int gm_virtio_alive(void) {
  uint8_t status;

  if (!gm_virtio_ready) return 0;
  status=gm_vcommon->device_status;
  if (status&(GM_VIRTIO_STATUS_NEEDS_RESET|GM_VIRTIO_STATUS_FAILED)) {
    gm_virtio_ready=0;
    return 0;
  }
  return 1;
}

static uint32_t gm_pci_read32(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off) {
  uint32_t addr;

  addr=(1U<<31)|((uint32_t)bus<<16)|((uint32_t)dev<<11)|
       ((uint32_t)fn<<8)|((uint32_t)off&0xfcU);
  gm_outl(GM_PCI_ADDR,addr);
  return gm_inl(GM_PCI_DATA);
}

static uint16_t gm_pci_read16(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off) {
  uint32_t value;

  value=gm_pci_read32(bus,dev,fn,off);
  return (uint16_t)((value>>((off&2U)*8U))&0xffffU);
}

static uint8_t gm_pci_read8(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off) {
  uint32_t value;

  value=gm_pci_read32(bus,dev,fn,off);
  return (uint8_t)(value>>((off&3U)*8U));
}

static void gm_pci_write32(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off,uint32_t value) {
  uint32_t addr;

  addr=(1U<<31)|((uint32_t)bus<<16)|((uint32_t)dev<<11)|
       ((uint32_t)fn<<8)|((uint32_t)off&0xfcU);
  gm_outl(GM_PCI_ADDR,addr);
  gm_outl(GM_PCI_DATA,value);
}

static void gm_pci_write16(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t off,uint16_t value) {
  uint32_t old;
  uint32_t shift;
  uint32_t mask;

  old=gm_pci_read32(bus,dev,fn,off);
  shift=(uint32_t)(off&2U)*8U;
  mask=0xffffU<<shift;
  gm_pci_write32(bus,dev,fn,off,(old&~mask)|((uint32_t)value<<shift));
}

static int gm_virtio_find(uint8_t *bus,uint8_t *dev,uint8_t *fn) {
  uint8_t d;
  uint8_t f;
  uint8_t header;
  uint8_t maxfn;
  uint16_t vendor;
  uint16_t device;

  for (d=0;d<32;d++) {
    vendor=gm_pci_read16(0,d,0,0);
    if (vendor==0xffffU) continue;
    header=gm_pci_read8(0,d,0,0x0e);
    maxfn=(header&0x80U)?8U:1U;
    for (f=0;f<maxfn;f++) {
      vendor=gm_pci_read16(0,d,f,0);
      if (vendor==0xffffU) continue;
      device=gm_pci_read16(0,d,f,2);
      if (vendor==GM_VIRTIO_VENDOR && device==GM_VIRTIO_NET_DEVICE) {
        *bus=0;
        *dev=d;
        *fn=f;
        return 1;
      }
    }
  }
  return 0;
}

static uint64_t gm_pci_bar(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t index) {
  uint32_t low;
  uint32_t high;
  uint32_t type;

  if (index>=6U) return 0;
  low=gm_pci_read32(bus,dev,fn,(uint8_t)(0x10U+index*4U));
  if (!low || low==0xffffffffU || (low&1U)) return 0;
  type=(low>>1)&3U;
  if (type==0U) return (uint64_t)(low&~0x0fU);
  if (type!=2U || index>=5U) return 0;
  high=gm_pci_read32(bus,dev,fn,(uint8_t)(0x14U+index*4U));
  return ((uint64_t)high<<32)|(uint64_t)(low&~0x0fU);
}

static int gm_virtio_caps(uint8_t bus,uint8_t dev,uint8_t fn,struct gm_virtio_cap *common,
                          struct gm_virtio_cap *notify,struct gm_virtio_cap *device) {
  struct gm_virtio_cap *cap;
  uint8_t pos;
  uint8_t next;
  uint8_t id;
  uint8_t len;
  uint8_t type;
  uint8_t bar;
  int count;

  gm_memset(common,0,sizeof(*common));
  gm_memset(notify,0,sizeof(*notify));
  gm_memset(device,0,sizeof(*device));
  if (!(gm_pci_read16(bus,dev,fn,0x06)&0x10U)) return 0;
  pos=(uint8_t)(gm_pci_read8(bus,dev,fn,0x34)&0xfcU);
  for (count=0;pos>=0x40U && count<48;count++) {
    id=gm_pci_read8(bus,dev,fn,pos);
    next=(uint8_t)(gm_pci_read8(bus,dev,fn,(uint8_t)(pos+1U))&0xfcU);
    len=gm_pci_read8(bus,dev,fn,(uint8_t)(pos+2U));
    type=gm_pci_read8(bus,dev,fn,(uint8_t)(pos+3U));
    bar=gm_pci_read8(bus,dev,fn,(uint8_t)(pos+4U));
    cap=0;
    if (id==GM_VIRTIO_CAP_ID && len>=16U) {
      if (type==GM_VIRTIO_CAP_COMMON) cap=common;
      else if (type==GM_VIRTIO_CAP_NOTIFY && len>=20U) cap=notify;
      else if (type==GM_VIRTIO_CAP_DEVICE) cap=device;
      if (cap && !cap->found) {
        cap->bar=bar;
        cap->offset=gm_pci_read32(bus,dev,fn,(uint8_t)(pos+8U));
        cap->length=gm_pci_read32(bus,dev,fn,(uint8_t)(pos+12U));
        cap->multiplier=type==GM_VIRTIO_CAP_NOTIFY?
                        gm_pci_read32(bus,dev,fn,(uint8_t)(pos+16U)):0;
        cap->found=1;
      }
    }
    if (!next || next==pos) break;
    pos=next;
  }
  return common->found && notify->found && device->found;
}

static void *gm_virtio_map_cap(uint8_t bus,uint8_t dev,uint8_t fn,const struct gm_virtio_cap *cap,
                               uint64_t virt,uint32_t minimum) {
  uint64_t bar;
  uint64_t phys;
  uint64_t page;
  uint64_t delta;
  uint64_t span;
  uint64_t off;

  if (!cap->found || cap->bar>=6U || cap->length<minimum || cap->length>GM_VIRTIO_MAP_MAX) return 0;
  bar=gm_pci_bar(bus,dev,fn,cap->bar);
  if (!bar) return 0;
  phys=bar+(uint64_t)cap->offset;
  if (phys<bar || phys+(uint64_t)cap->length<phys) return 0;
  page=phys&~(GM_PAGE_SIZE-1ULL);
  delta=phys-page;
  span=delta+(uint64_t)cap->length;
  if (span>GM_VIRTIO_MAP_MAX) return 0;
  for (off=0;off<span;off+=GM_PAGE_SIZE) {
    if (!gm_map_page(virt+off,page+off,GM_MMIO_FLAGS)) return 0;
  }
  return (void *)(virt+delta);
}

static int gm_virtio_reset(void) {
  uint32_t i;

  gm_vcommon->device_status=0;
  gm_barrier();
  for (i=0;i<100000U;i++) {
    if (!gm_vcommon->device_status) return 1;
    __asm__ volatile("pause");
  }
  return 0;
}

static int gm_virtio_features(void) {
  uint32_t low;
  uint32_t high;
  uint8_t status;

  gm_vcommon->device_status=GM_VIRTIO_STATUS_ACK;
  gm_vcommon->device_status|=GM_VIRTIO_STATUS_DRIVER;
  gm_vcommon->device_feature_select=0;
  low=gm_vcommon->device_feature;
  gm_vcommon->device_feature_select=1;
  high=gm_vcommon->device_feature;
  if (!(low&GM_VIRTIO_NET_F_MAC) || !(high&GM_VIRTIO_F_VERSION_1)) return 0;
  gm_vcommon->driver_feature_select=0;
  gm_vcommon->driver_feature=GM_VIRTIO_NET_F_MAC;
  gm_vcommon->driver_feature_select=1;
  gm_vcommon->driver_feature=GM_VIRTIO_F_VERSION_1;
  status=gm_vcommon->device_status;
  gm_vcommon->device_status=(uint8_t)(status|GM_VIRTIO_STATUS_FEATURES_OK);
  gm_barrier();
  return (gm_vcommon->device_status&GM_VIRTIO_STATUS_FEATURES_OK)!=0;
}

static int gm_virtq_init(struct gm_virtq *queue,uint16_t index,volatile uint8_t *notify_base,
                         uint32_t notify_len,uint32_t multiplier) {
  uint64_t phys;
  uint8_t *mem;
  uint64_t notify_off;

  gm_vcommon->queue_select=index;
  if (gm_vcommon->queue_enable || gm_vcommon->queue_size<GM_VIRTIO_QUEUE_SIZE) return 0;
  phys=gm_page_alloc();
  mem=(uint8_t *)gm_phys(phys);
  queue->desc=(struct gm_virtq_desc *)(mem+GM_VIRTQ_DESC_OFF);
  queue->avail=(struct gm_virtq_avail *)(mem+GM_VIRTQ_AVAIL_OFF);
  queue->used=(struct gm_virtq_used *)(mem+GM_VIRTQ_USED_OFF);
  queue->last_used=0;
  gm_vcommon->queue_size=GM_VIRTIO_QUEUE_SIZE;
  gm_vcommon->queue_msix_vector=GM_VIRTQ_NO_VECTOR;
  gm_vcommon->queue_desc=phys+GM_VIRTQ_DESC_OFF;
  gm_vcommon->queue_driver=phys+GM_VIRTQ_AVAIL_OFF;
  gm_vcommon->queue_device=phys+GM_VIRTQ_USED_OFF;
  notify_off=(uint64_t)gm_vcommon->queue_notify_off*(uint64_t)multiplier;
  if (notify_off+sizeof(uint16_t)>notify_len) return 0;
  queue->notify=(volatile uint16_t *)(notify_base+notify_off);
  gm_barrier();
  gm_vcommon->queue_enable=1;
  gm_barrier();
  return gm_vcommon->queue_enable!=0;
}

static void gm_virtq_notify(struct gm_virtq *queue,uint16_t index) {
  gm_barrier();
  *queue->notify=index;
}

static int gm_virtio_reap_tx(void) {
  uint16_t used;
  uint32_t id;

  if (!gm_virtio_alive()) return 0;
  used=gm_vtx.used->idx;
  gm_acquire();
  while (gm_vtx.last_used!=used) {
    id=gm_vtx.used->ring[gm_vtx.last_used%GM_VIRTIO_QUEUE_SIZE].id;
    if (id>=GM_VIRTIO_QUEUE_SIZE || gm_tx_free[id]) {
      gm_vcommon->device_status|=GM_VIRTIO_STATUS_FAILED;
      gm_virtio_ready=0;
      return 0;
    }
    gm_tx_free[id]=1;
    gm_vtx.last_used++;
    used=gm_vtx.used->idx;
    gm_acquire();
  }
  return 1;
}

int gm_virtio_net_init(uint8_t mac[6]) {
  struct gm_virtio_cap common;
  struct gm_virtio_cap notify;
  struct gm_virtio_cap device;
  volatile uint8_t *notify_base;
  uint8_t bus;
  uint8_t dev;
  uint8_t fn;
  uint8_t generation;
  uint8_t check;
  uint16_t command;
  uint16_t i;
  uint8_t attempt;
  uint64_t rx_page;
  uint64_t tx_page;

  gm_virtio_ready=0;
  if (!mac || !gm_virtio_find(&bus,&dev,&fn)) return 0;
  command=gm_pci_read16(bus,dev,fn,4);
  gm_pci_write16(bus,dev,fn,4,(uint16_t)(command|0x0006U));
  if (!gm_virtio_caps(bus,dev,fn,&common,&notify,&device)) return 0;
  gm_vcommon=(volatile struct gm_virtio_common *)gm_virtio_map_cap(bus,dev,fn,&common,
                                                                   GM_VIRTIO_MAP_COMMON,56U);
  notify_base=(volatile uint8_t *)gm_virtio_map_cap(bus,dev,fn,&notify,GM_VIRTIO_MAP_NOTIFY,2U);
  gm_vdevice=(volatile uint8_t *)gm_virtio_map_cap(bus,dev,fn,&device,GM_VIRTIO_MAP_DEVICE,6U);
  if (!gm_vcommon || !notify_base || !gm_vdevice) return 0;
  if (!gm_virtio_reset() || !gm_virtio_features()) goto fail;
  if (gm_vcommon->num_queues<2U) goto fail;
  if (!gm_virtq_init(&gm_vrx,GM_VIRTIO_RX_QUEUE,notify_base,notify.length,notify.multiplier)) goto fail;
  if (!gm_virtq_init(&gm_vtx,GM_VIRTIO_TX_QUEUE,notify_base,notify.length,notify.multiplier)) goto fail;

  rx_page=0;
  tx_page=0;
  for (i=0;i<GM_VIRTIO_QUEUE_SIZE;i++) {
    if (!(i&1U)) {
      rx_page=gm_page_alloc();
      tx_page=gm_page_alloc();
    }
    gm_vrx.desc[i].addr=rx_page+(uint64_t)(i&1U)*GM_VIRTIO_BUF_SIZE;
    gm_rx_buf[i]=(uint8_t *)gm_phys(gm_vrx.desc[i].addr);
    gm_vrx.desc[i].len=GM_VIRTIO_BUF_SIZE;
    gm_vrx.desc[i].flags=GM_VIRTQ_DESC_F_WRITE;
    gm_vrx.desc[i].next=0;
    gm_vrx.avail->ring[i]=i;

    gm_vtx.desc[i].addr=tx_page+(uint64_t)(i&1U)*GM_VIRTIO_BUF_SIZE;
    gm_tx_buf[i]=(uint8_t *)gm_phys(gm_vtx.desc[i].addr);
    gm_vtx.desc[i].len=0;
    gm_vtx.desc[i].flags=0;
    gm_vtx.desc[i].next=0;
    gm_tx_free[i]=1;
  }
  gm_vrx.avail->flags=GM_VIRTQ_AVAIL_F_NO_INTERRUPT;
  gm_vtx.avail->flags=GM_VIRTQ_AVAIL_F_NO_INTERRUPT;
  gm_vrx.avail->idx=GM_VIRTIO_QUEUE_SIZE;
  gm_vtx.avail->idx=0;
  gm_tx_next=0;
  for (attempt=0;attempt<8U;attempt++) {
    generation=gm_vcommon->config_generation;
    gm_acquire();
    for (i=0;i<6U;i++) mac[i]=gm_vdevice[i];
    gm_acquire();
    check=gm_vcommon->config_generation;
    if (generation==check) break;
  }
  if (attempt==8U || !(mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5])) goto fail;
  gm_vcommon->device_status|=GM_VIRTIO_STATUS_DRIVER_OK;
  gm_barrier();
  if (!(gm_vcommon->device_status&GM_VIRTIO_STATUS_DRIVER_OK)) goto fail;
  gm_virtio_ready=1;
  gm_virtq_notify(&gm_vrx,GM_VIRTIO_RX_QUEUE);
  return 1;

fail:
  if (gm_vcommon) gm_vcommon->device_status|=GM_VIRTIO_STATUS_FAILED;
  return 0;
}

int gm_virtio_net_send(const uint8_t *frame,uint16_t len) {
  uint16_t slot;
  uint16_t avail;
  uint16_t i;

  if (!frame || !len || len>1518U || !gm_virtio_reap_tx()) return 0;
  slot=gm_tx_next;
  for (i=0;i<GM_VIRTIO_QUEUE_SIZE;i++) {
    if (gm_tx_free[slot]) break;
    slot=(uint16_t)((slot+1U)%GM_VIRTIO_QUEUE_SIZE);
  }
  if (i==GM_VIRTIO_QUEUE_SIZE) return 0;
  gm_tx_free[slot]=0;
  gm_memset(gm_tx_buf[slot],0,GM_VIRTIO_NET_HDR);
  gm_memcpy(gm_tx_buf[slot]+GM_VIRTIO_NET_HDR,frame,len);
  gm_vtx.desc[slot].len=(uint32_t)len+GM_VIRTIO_NET_HDR;
  gm_vtx.desc[slot].flags=0;
  avail=gm_vtx.avail->idx;
  gm_vtx.avail->ring[avail%GM_VIRTIO_QUEUE_SIZE]=slot;
  gm_release();
  gm_vtx.avail->idx=(uint16_t)(avail+1U);
  gm_tx_next=(uint16_t)((slot+1U)%GM_VIRTIO_QUEUE_SIZE);
  gm_virtq_notify(&gm_vtx,GM_VIRTIO_TX_QUEUE);
  return 1;
}

int gm_virtio_net_poll(uint8_t *frame,uint16_t cap,uint16_t *len) {
  uint16_t used;
  uint16_t avail;
  uint16_t frame_len;
  uint32_t id;
  uint32_t bytes;
  int valid;

  if (!frame || !len || !gm_virtio_reap_tx()) return -1;
  for (;;) {
    used=gm_vrx.used->idx;
    gm_acquire();
    if (gm_vrx.last_used==used) return 0;
    id=gm_vrx.used->ring[gm_vrx.last_used%GM_VIRTIO_QUEUE_SIZE].id;
    bytes=gm_vrx.used->ring[gm_vrx.last_used%GM_VIRTIO_QUEUE_SIZE].len;
    gm_vrx.last_used++;
    if (id>=GM_VIRTIO_QUEUE_SIZE) {
      gm_vcommon->device_status|=GM_VIRTIO_STATUS_FAILED;
      gm_virtio_ready=0;
      return -1;
    }
    valid=bytes>=GM_VIRTIO_NET_HDR && bytes<=GM_VIRTIO_BUF_SIZE &&
          bytes-GM_VIRTIO_NET_HDR<=cap;
    frame_len=valid?(uint16_t)(bytes-GM_VIRTIO_NET_HDR):0;
    if (valid) gm_memcpy(frame,gm_rx_buf[id]+GM_VIRTIO_NET_HDR,frame_len);
    avail=gm_vrx.avail->idx;
    gm_vrx.avail->ring[avail%GM_VIRTIO_QUEUE_SIZE]=(uint16_t)id;
    gm_release();
    gm_vrx.avail->idx=(uint16_t)(avail+1U);
    gm_virtq_notify(&gm_vrx,GM_VIRTIO_RX_QUEUE);
    if (!valid) continue;
    *len=frame_len;
    return 1;
  }
}
