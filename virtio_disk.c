//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"
#include "disk.h"
#include "proc.h"
// RAID prototypes
void raid1_write(int logical, void *data);
void raid1_read(int logical, void *data);
void raid5_write(int logical, void *data);
void raid5_read(int logical, void *data);

struct disk_req {
  int type;              // READ / WRITE
  int logical_block;
  void *data;
  struct proc *p;
  int priority;
};
// the address of virtio mmio register r.
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))
struct disk_req disk_queue[MAX_REQ];
int qsize = 0;

static int abs(int x){
  return x < 0 ? -x : x;
}
int disk_policy = 0;   // 0 = FCFS, 1 = SSTF
int disk_reads = 0;
int disk_writes = 0;
int total_latency = 0;
int total_requests = 0;

int current_head = 0;
int devs[NDISK] = {1, 2, 3, 4};   // simulated disks
int compute_latency(int block){
  return abs(current_head - block) + ROT_DELAY;
}
int get_parity_disk(int block){
  return block % NDISK;
}
struct disk_req disk_queue[MAX_REQ];

#define RAID0 0
#define RAID1 1
#define RAID5 2
#define FCFS  0
#define SSTF  1

int raid_mode = RAID0;   // default

static struct disk {
  // a set (not a ring) of DMA descriptors, with which the
  // driver tells the device where to read and write individual
  // disk operations. there are NUM descriptors.
  // most commands consist of a "chain" (a linked list) of a couple of
  // these descriptors.
  struct virtq_desc *desc;

  // a ring in which the driver writes descriptor numbers
  // that the driver would like the device to process.  it only
  // includes the head descriptor of each chain. the ring has
  // NUM elements.
  struct virtq_avail *avail;

  // a ring in which the device writes descriptor numbers that
  // the device has finished processing (just the head of each chain).
  // there are NUM used ring entries.
  struct virtq_used *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  


  struct {
    struct buf *b;
    char status;
  } info[NUM];


  

  // disk command headers.
  // one-for-one with descriptors, for convenience.
  struct virtio_blk_req ops[NUM];
  
  struct spinlock vdisk_lock;
  
} disk;

void
virtio_disk_init(void)
{
  uint32 status = 0;

  initlock(&disk.vdisk_lock, "virtio_disk");

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 2 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk");
  }
  
  // reset device
  *R(VIRTIO_MMIO_STATUS) = status;

  // set ACKNOWLEDGE status bit
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  // set DRIVER status bit
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // re-read status to ensure FEATURES_OK is set.
  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // initialize queue 0.
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  // ensure queue 0 is not in use.
  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // check maximum queue size.
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");

  // allocate and zero queue memory.
  disk.desc = kalloc();
  disk.avail = kalloc();
  disk.used = kalloc();
  if(!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // set queue size.
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // write physical addresses.
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

  // queue is ready.
  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // all NUM descriptors start out unused.
  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(disk.free[i])
    panic("free_desc 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

// free a chain of descriptors.
static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / 512);

  acquire(&disk.vdisk_lock);

  // the spec's Section 5.2 says that legacy block operations use
  // three descriptors: one for type/reserved/sector, one for the
  // data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc(idx) == 0) {
      break;
    }
    sleep(&disk.free[0], &disk.vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

  if(write)
    buf0->type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0->type = VIRTIO_BLK_T_IN; // read the disk
  buf0->reserved = 0;
  buf0->sector = sector;

  disk.desc[idx[0]].addr = (uint64) buf0;
  disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = (uint64) b->data;
  disk.desc[idx[1]].len = BSIZE;
  if(write)
    disk.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0xff; // device writes 0 on success
  disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk.desc[idx[2]].next = 0;

  // record struct buf for virtio_disk_intr().
  b->disk = 1;
  disk.info[idx[0]].b = b;

  // tell the device the first index in our chain of descriptors.
  disk.avail->ring[disk.avail->idx % NUM] = idx[0];

  __sync_synchronize();

  // tell the device another avail ring entry is available.
  disk.avail->idx += 1; // not % NUM ...

  __sync_synchronize();

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 1) {
    sleep(b, &disk.vdisk_lock);
  }

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  release(&disk.vdisk_lock);
}

void
virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  // the device won't raise another interrupt until we tell it
  // we've seen this interrupt, which the following line does.
  // this may race with the device writing new entries to
  // the "used" ring, in which case we may process the new
  // completion entries in this interrupt, and have nothing to do
  // in the next interrupt, which is harmless.
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  // the device increments disk.used->idx when it
  // adds an entry to the used ring.

  while(disk.used_idx != disk.used->idx){
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *b = disk.info[id].b;
    b->disk = 0;   // disk is done with buf
    wakeup(b);

    disk.used_idx += 1;
  }

  release(&disk.vdisk_lock);
}
void raid0_map(int logical, int *disk, int *block){
  *disk = logical % NDISK;
  *block = logical / NDISK;
}
void submit_disk_request(int type, int swap_idx, int block_offset, void *data, struct proc *p){
  int logical_block = swap_idx * BLOCKS_PER_PAGE + block_offset;

  disk_queue[qsize].type = type;
   disk_queue[qsize].logical_block = logical_block;
   disk_queue[qsize].data = data;
   disk_queue[qsize].p = p;
   disk_queue[qsize].priority = p->level;
qsize++;
}
int pick_request(){
  int best = -1;

  for(int i = 0; i < qsize; i++){
    if(best == -1){
      best = i;
      continue;
    }

    struct disk_req *r1 = &disk_queue[i];
    struct disk_req *r2 = &disk_queue[best];
    
    if(r1->p->level < r2->p->level){
      best = i;
      continue;
    }

    if(r1->p->level > r2->p->level){
      continue;
    }

    

    if(disk_policy == 0){
      // FCFS → keep earlier request
      continue;
    }
    else if(disk_policy == 1){
      // SSTF → choose closer block
      int d1 = abs(current_head - r1->logical_block);
      int d2 = abs(current_head - r2->logical_block);

      if(d1 < d2){
        best = i;
      }
    }
  }

  return best;
}
void map_block(int logical, int *disk, int *block){
  *disk = logical % NDISK;
  *block = logical / NDISK;
}
void process_requests(){
  while(qsize > 0){
    int idx = pick_request();
    struct disk_req req = disk_queue[idx];
     disk_queue[idx] = disk_queue[--qsize];
    int lat = compute_latency(req.logical_block);
    total_latency += lat;
    total_requests++;

    if(req.type == READ)
      disk_reads++;
    else
      disk_writes++;

    // ───── RAID HANDLING ─────
    if(raid_mode == RAID0){
      int disk, block;
      raid0_map(req.logical_block, &disk, &block);

      struct buf *b = bread(devs[disk], block);

      if(req.type == WRITE){
        memmove(b->data, req.data, BSIZE);
        bwrite(b);
      } else {
        memmove(req.data, b->data, BSIZE);
      }

      brelse(b);
    }

    else if(raid_mode == RAID1){
      if(req.type == WRITE){
        raid1_write(req.logical_block, req.data);
      } else {
        raid1_read(req.logical_block, req.data);
      }
    }

    else if(raid_mode == RAID5){
      if(req.type == WRITE){
        raid5_write(req.logical_block, req.data);
      } else {
        raid5_read(req.logical_block, req.data);
      }
    }

    current_head = req.logical_block;
  }
}
int set_disk_policy(int policy){
  if(policy != FCFS && policy != SSTF)
    return -1;

  disk_policy = policy;
  return 0;
}
void raid5_map(int logical, int *disk, int *block){
  int stripe = logical / (NDISK - 1);
  int offset = logical % (NDISK - 1);

  int parity_disk = stripe % NDISK;

  int data_disk = 0;
  for(int d = 0; d < NDISK; d++){
    if(d == parity_disk) continue;

    if(data_disk == offset){
      *disk = d;
      *block = stripe;
      return;
    }
    data_disk++;
  }
}
void raid5_read(int logical, void *data){
  int stripe = logical / (NDISK - 1);
  int parity_disk = stripe % NDISK;

  int offset = logical % (NDISK - 1);
  int data_disk_index = 0;

  for(int d = 0; d < NDISK; d++){
    if(d == parity_disk) continue;

    if(data_disk_index == offset){
      struct buf *b = bread(1, stripe);
      memmove(data, b->data, BSIZE);
      brelse(b);
      return;
    }
    data_disk_index++;
  }
}
void raid5_write(int logical, void *data){
  int stripe = logical / (NDISK - 1);
  int parity_disk = stripe % NDISK;

  char parity[BSIZE];
  memset(parity, 0, BSIZE);

  int offset = logical % (NDISK - 1);
  int data_disk_index = 0;

  // First pass: compute parity
  for(int d = 0; d < NDISK; d++){
    if(d == parity_disk) continue;

    char temp[BSIZE];

    if(data_disk_index == offset){
      // this is the block we are writing
      memmove(temp, data, BSIZE);
    } else {
      // read existing data
      struct buf *b = bread(1, stripe);
      memmove(temp, b->data, BSIZE);
      brelse(b);
    }

    // XOR into parity
    for(int i = 0; i < BSIZE; i++){
      parity[i] ^= temp[i];
    }

    data_disk_index++;
  }

  // Second pass: write data block
  data_disk_index = 0;
  for(int d = 0; d < NDISK; d++){
    if(d == parity_disk) continue;

    if(data_disk_index == offset){
      struct buf *b = bread(1, stripe);
      memmove(b->data, data, BSIZE);
      bwrite(b);
      brelse(b);
      break;
    }
    data_disk_index++;
  }

  // Third: write parity block
  struct buf *pb = bread(1, stripe);
  memmove(pb->data, parity, BSIZE);
  bwrite(pb);
  brelse(pb);
}
void raid5_read_reconstruct(int logical, void *data, int failed_disk){
  int stripe = logical / (NDISK - 1);

  char result[BSIZE];
  memset(result, 0, BSIZE);

  for(int d = 0; d < NDISK; d++){
    if(d == failed_disk) continue;

    struct buf *b = bread(1, stripe);

    for(int i = 0; i < BSIZE; i++){
      result[i] ^= b->data[i];
    }

    brelse(b);
  }

  memmove(data, result, BSIZE);
}

void raid1_write(int logical, void *data){
  int disk1 = logical % NDISK;
  int disk2 = (disk1 + 1) % NDISK;   // mirror

  int block = logical / NDISK;

  struct buf *b1 = bread(devs[disk1], block);
  memmove(b1->data, data, BSIZE);
  bwrite(b1);
  brelse(b1);

  struct buf *b2 = bread(devs[disk2], block);
  memmove(b2->data, data, BSIZE);
  bwrite(b2);
  brelse(b2);
}
// ===================== RAID 1 READ =====================
void raid1_read(int logical, void *data){
  int disk1 = logical % NDISK;
  int block = logical / NDISK;

  // just read from one disk (simplest)
  struct buf *b = bread(devs[disk1], block);
  memmove(data, b->data, BSIZE);
  brelse(b);
}