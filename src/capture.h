#ifndef CAPTURE_H
#define CAPTURE_H

#include "frame.h"

typedef struct capture_s capture_t;

capture_t *cap_open(const char *device, int w, int h, int fps, uint32_t fmt);
int  cap_dequeue(capture_t *c, frame_t *f);  /* blocking, fills f */
void cap_queue(capture_t *c);                /* return buffer to driver */
void cap_close(capture_t *c);

#endif
