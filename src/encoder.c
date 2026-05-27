/*
 * encoder.c - MPP hardware encoder for RK3568
 *
 * Uses Rockchip MPP (Media Process Platform) API.
 * Link with: -lrockchip_mpp
 *
 * Simplified API: enc_open / enc_feed / enc_close
 */

#include "encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

struct encoder_s {
    MppCtx          ctx;
    MppApi         *mpi;
    MppBufferGroup  group;
    int             w, h, fps, bitrate, gop;
    size_t          frame_size;
    MppCodingType   coding;
    int             frame_count;
};

encoder_t *enc_open(int w, int h, int fps, int bitrate, const char *codec)
{
    encoder_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->w  = w;
    e->h  = h;
    e->fps = fps;
    e->bitrate = bitrate ? bitrate : w * h * 2;
    e->gop = fps;
    e->frame_size = w * h * 3 / 2;
    e->coding = (codec && strlen(codec) >= 4 && codec[3] == '5')
                ? MPP_VIDEO_CodingHEVC
                : MPP_VIDEO_CodingAVC;

    MPP_RET ret;

    ret = mpp_create(&e->ctx, &e->mpi);
    if (ret != MPP_OK) { free(e); return NULL; }

    ret = mpp_init(e->ctx, MPP_CTX_ENC, e->coding);
    if (ret != MPP_OK) { mpp_destroy(e->ctx); free(e); return NULL; }

    /* Prep config */
    MppEncPrepCfg prep;
    memset(&prep, 0, sizeof(prep));
    prep.width      = w;
    prep.height     = h;
    prep.hor_stride = w;
    prep.ver_stride = h;
    prep.format     = MPP_FMT_YUV420SP;
    prep.rotation   = MPP_ENC_ROT_0;
    e->mpi->control(e->ctx, MPP_ENC_SET_PREP_CFG, &prep);

    /* Rate control */
    MppEncRcCfg rc;
    memset(&rc, 0, sizeof(rc));
    rc.rc_mode    = MPP_ENC_RC_MODE_CBR;
    rc.bps_target = e->bitrate;
    rc.bps_max    = e->bitrate * 3 / 2;
    rc.bps_min    = e->bitrate / 2;
    rc.gop        = e->gop;
    e->mpi->control(e->ctx, MPP_ENC_SET_RC_CFG, &rc);

    /* Codec config */
    MppEncCodecCfg cc;
    memset(&cc, 0, sizeof(cc));
    cc.coding = e->coding;
    if (e->coding == MPP_VIDEO_CodingAVC) {
        cc.h264.change  = MPP_ENC_H264_CFG_CHANGE_PROFILE |
                          MPP_ENC_H264_CFG_CHANGE_ENTROPY;
        cc.h264.level   = 40;
        cc.h264.profile = 100;
        cc.h264.entropy_coding_mode = 1;
    } else {
        cc.h265.change = 0;
        cc.h265.level  = 93;
    }
    e->mpi->control(e->ctx, MPP_ENC_SET_CODEC_CFG, &cc);

    /* Buffer group */
    ret = mpp_buffer_group_get_internal(&e->group, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK) {
        mpp_destroy(e->ctx);
        free(e);
        return NULL;
    }
    mpp_buffer_group_limit_config(e->group, e->frame_size, 4);

    printf("[enc] %dx%d@%d %s bitrate=%d\n", w, h, fps, codec, e->bitrate);
    return e;
}

int enc_feed(encoder_t *e, const frame_t *f, uint8_t **out, size_t *olen)
{
    *out  = NULL;
    *olen = 0;

    if (!e || !f || !f->ptr) return -1;

    MppBuffer buf = NULL;
    if (mpp_buffer_get(e->group, &buf, e->frame_size) != MPP_OK || !buf)
        return -1;

    /* Copy NV12 frame data into MPP buffer, stripping V4L2 stride if needed. */
    uint8_t *dst = (uint8_t *)mpp_buffer_get_ptr(buf);
    const uint8_t *src = (const uint8_t *)f->ptr;
    if ((int)f->stride == e->w) {
        memcpy(dst, src, e->frame_size);
    } else {
        for (int r = 0; r < e->h; r++) {
            memcpy(dst, src, e->w);
            dst += e->w;
            src += f->stride;
        }
        src = (const uint8_t *)f->ptr + f->stride * e->h;
        for (int r = 0; r < e->h / 2; r++) {
            memcpy(dst, src, e->w);
            dst += e->w;
            src += f->stride;
        }
    }

    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_width(frame, e->w);
    mpp_frame_set_height(frame, e->h);
    mpp_frame_set_hor_stride(frame, e->w);
    mpp_frame_set_ver_stride(frame, e->h);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_eos(frame, 0);
    mpp_frame_set_pts(frame, f->pts);

    MPP_RET ret = e->mpi->encode_put_frame(e->ctx, frame);
    mpp_frame_deinit(&frame);
    mpp_buffer_put(buf);

    if (ret != MPP_OK) return -1;

    MppPacket pkt = NULL;
    ret = e->mpi->encode_get_packet(e->ctx, &pkt);
    if (ret == MPP_OK && pkt) {
        size_t len = mpp_packet_get_length(pkt);
        if (len > 0) {
            *out  = malloc(len);
            if (*out) {
                memcpy(*out, mpp_packet_get_data(pkt), len);
                *olen = len;
            }
        }
        mpp_packet_deinit(&pkt);
    }

    e->frame_count++;
    return 0;
}

void enc_close(encoder_t *e)
{
    if (!e) return;

    /* Send EOS frame and drain remaining packets */
    MppFrame f = NULL;
    mpp_frame_init(&f);
    mpp_frame_set_eos(f, 1);
    e->mpi->encode_put_frame(e->ctx, f);
    mpp_frame_deinit(&f);

    MppPacket p = NULL;
    int drain = 10;
    while (drain-- > 0 &&
           e->mpi->encode_get_packet(e->ctx, &p) == MPP_OK) {
        if (p) mpp_packet_deinit(&p);
    }

    e->mpi->reset(e->ctx);
    mpp_destroy(e->ctx);
    if (e->group) mpp_buffer_group_put(e->group);
    free(e);
}
