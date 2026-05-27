#ifndef ENCODER_H
#define ENCODER_H

#include <stddef.h>
#include "frame.h"

typedef struct encoder_s encoder_t;

encoder_t *enc_open(int w, int h, int fps, int bitrate, const char *codec);
int  enc_feed(encoder_t *e, const frame_t *f, uint8_t **out, size_t *olen);
void enc_close(encoder_t *e);

#endif
