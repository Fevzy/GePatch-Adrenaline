#ifndef GETAP_SHARED_H
#define GETAP_SHARED_H

/* GE tap: the PSP-side plugin (ge_tap.prx) records every GE list submission
 * and frame flip into a ring in PSP memory; Adrenaline's Vita side reads the
 * lists from PSP memory as they are submitted. The PSP side writes through
 * uncached addresses, so the ring is in RAM without cache maintenance there.
 *
 * The ring is allocated in the PSP kernel partition (extra RAM is used by
 * games and cleared by Adrenaline); its address is published in Adrenaline's
 * own page, next to GePatch's DRAW_NATIVE word.
 *
 * VRAM belongs to the PSP's GE and is not visible from the Vita side. What
 * games DMA into it is reported as an event (the Vita side copies the same
 * source from RAM); pages written otherwise are forwarded on request: the Vita
 * side sets a page's bit in vram_request, the PSP side copies the page into a
 * slot of its pool on the next frame flip and publishes the slot in vram_slot,
 * then refreshes a few forwarded pages per frame. */
#define GETAP_POINTER 0xABCDEF10
#define GETAP_MAGIC 0x50415447 /* "GTAP" */
#define GETAP_VERSION 6
#define GETAP_RING 2048 /* events, a power of two */
/* The VRAM page pool lives in extra RAM where GePatch keeps its vertex pool:
 * free while a game runs. The PSP kernel partition is too small for it (save
 * data crypto failed with 1 MiB taken there). */
#define GETAP_POOL 0x0A600000
#define GETAP_POOL_PAGES 512 /* 2 MiB, up to GePatch's render list */
#define GETAP_VRAM_PAGE 0x1000
#define GETAP_VRAM_PAGES (0x200000 / GETAP_VRAM_PAGE)

enum GetapEventType {
  GETAP_EV_ENQUEUE = 1,      /* a = qid, b = list, c = stall */
  GETAP_EV_ENQUEUE_HEAD = 2, /* a = qid, b = list, c = stall */
  GETAP_EV_STALL = 3,        /* a = qid, b = new stall */
  GETAP_EV_FRAME = 4,        /* a = framebuffer, b = width | format << 16, c = sync */
  GETAP_EV_DMA = 5,          /* a = VRAM destination, b = source, c = size */
};

typedef struct {
  unsigned int type, a, b, c;
} GetapEvent;

typedef struct {
  unsigned int magic, version;
  volatile unsigned int write; /* events written, wraps at 2^32 */
  unsigned int lost;           /* enqueue failures seen by the PSP side */
  unsigned int pool;           /* PSP address of the VRAM page pool */
  unsigned int pool_pages;
  volatile unsigned int copy_us;   /* PSP time spent copying pages, total */
  volatile unsigned int copied;    /* pages copied, total */
  volatile unsigned int pool_full; /* requests refused, total */
  unsigned int dma_hooked;
  unsigned int reserved[6];
  volatile unsigned int vram_request[GETAP_VRAM_PAGES / 32]; /* Vita -> PSP */
  /* Vita -> PSP: events the Vita side has read. When the game waits for the
   * GE, the PSP side also waits for this (bounded), so what the lists use is
   * read before the game changes it. A cache line of its own. */
  volatile unsigned int vita_read;
  unsigned int vita_pad[15];
  volatile unsigned short vram_slot[GETAP_VRAM_PAGES];       /* PSP -> Vita: slot + 1 */
  GetapEvent events[GETAP_RING];
} GetapRing;

#endif
