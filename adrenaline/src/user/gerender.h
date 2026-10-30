#ifndef __GERENDER_H__
#define __GERENDER_H__

#include <stdint.h>

/*
  GE renderer: draws the frames the GE tap follows with the Vita GPU, at the
  Vita's resolution. The tap thread feeds it every GE command it walks
  (grCommand) and every frame flip (grEndFrame); the draw thread presents the
  newest complete frame inside its scene (grPresent).
*/

/* Reads PSP memory (RAM, or VRAM pages the PSP side forwards) into dst.
   Returns 0 if part of it is not available yet. Provided by getap.c. */
int tapReadBlock(uint32_t addr, void *dst, uint32_t size);

void grInit(void);
void grReset(void);
/* One GE command, in GE execution order. offset is the list's ORIGIN /
   OFFSETADDR: vertex and index addresses are relative to it, like jumps. */
void grCommand(uint32_t cmd, uint32_t data, uint32_t offset);
/* The game shows the framebuffer at topaddr. */
void grEndFrame(uint32_t topaddr);
/* A frame is waiting or shown: the draw thread must draw. */
int grHasFrame(void);
/* Draws the frames the game finished into our render targets: call outside
   any scene, before the display scene. */
void grRender(void);
/* Draws the framebuffer the game shows into the current scene; 0 if none. */
int grPresent(void);
/* Statistics for the tap's report. */
void grReport(void);
/* Saves what is shown and what the PSP drew (BMP) for comparison: the tap
   thread, when R + SELECT is pressed. */
void grDump(void);

#endif
