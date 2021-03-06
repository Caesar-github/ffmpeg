/*
 * RockChip MPP Video Decoder
 * Copyright (c) 2017 Lionel CHAZALLON
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <libdrm/drm_fourcc.h>
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_mpi.h>
#include <time.h>
#include <unistd.h>

#include "avcodec.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"

#define RECEIVE_FRAME_TIMEOUT   100
#define FRAMEGROUP_MAX_FRAMES   16

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup frame_group;

    char first_frame;
    char first_packet;
    char eos_reached;
} RKMPPDecoder;

typedef struct {
    AVClass *av_class;
    AVBufferRef *decoder_ref;
} RKMPPDecodeContext;

typedef struct {
    MppFrame frame;
    AVBufferRef *decoder_ref;
} RKMPPFrameContext;

static MppCodingType ffrkmpp_get_codingtype(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:  return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:  return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_VP8:   return MPP_VIDEO_CodingVP8;
    default:                return MPP_VIDEO_CodingUnused;
    }
}

static int ffrkmpp_get_frameformat(MppFrameFormat mppformat)
{
    switch (mppformat) {
    case MPP_FMT_YUV420SP:          return DRM_FORMAT_NV12;
#ifdef DRM_FORMAT_NV12_10
    case MPP_FMT_YUV420SP_10BIT:    return DRM_FORMAT_NV12_10;
#endif
    default:                        return 0;
    }
}

static int ffrkmpp_write_data(AVCodecContext *avctx, uint8_t *buffer, int size, int64_t pts)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MPP_RET ret = MPP_NOK;
    MppPacket packet;

    // create the MPP packet
    ret = mpp_packet_init(&packet, buffer, size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_pts(packet, pts);

    if (!buffer)
        mpp_packet_set_eos(packet);

    ret = decoder->mpi->decode_put_packet(decoder->ctx, packet);
    if (ret != MPP_OK) {
        if (ret == MPP_ERR_BUFFER_FULL) {
            av_log(avctx, AV_LOG_DEBUG, "Buffer full writing %d bytes to decoder\n", size);
            ret = AVERROR(EAGAIN);
        } else
            ret = AVERROR_UNKNOWN;
    }
    else
        av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", size);

    mpp_packet_deinit(&packet);

    return ret;
}

static int ffrkmpp_close_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    av_buffer_unref(&rk_context->decoder_ref);
    return 0;
}

static void ffrkmpp_release_decoder(void *opaque, uint8_t *data)
{
    RKMPPDecoder *decoder = (RKMPPDecoder *)data;

    decoder->mpi->reset(decoder->ctx);
    mpp_destroy(decoder->ctx);
    decoder->ctx = NULL;

    if (decoder->frame_group) {
        mpp_buffer_group_put(decoder->frame_group);
        decoder->frame_group = NULL;
    }

    av_free(decoder);
}

static int ffrkmpp_init_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = NULL;
    MppCodingType codectype = MPP_VIDEO_CodingUnused;
    MPP_RET ret = MPP_NOK;
    RK_S64 paramS64;
    RK_S32 paramS32;

    if ((ret = ff_get_format(avctx, avctx->codec->pix_fmts)) < 0)
        return ret;

    avctx->pix_fmt = ret;

    // create a decoder and a ref to it
    decoder = av_mallocz(sizeof(RKMPPDecoder));
    if (!decoder) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    rk_context->decoder_ref = av_buffer_create((uint8_t *)decoder, sizeof(*decoder), ffrkmpp_release_decoder,
                                               NULL, AV_BUFFER_FLAG_READONLY);
    if (!rk_context->decoder_ref) {
        av_free(decoder);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP decoder.\n");

    codectype = ffrkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // Create the MPP context
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // initialize mpp
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // make decode calls blocking with a timeout
    paramS32 = MPP_POLL_BLOCK;
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_BLOCK, &paramS32);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set blocking mode on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    paramS64 = RECEIVE_FRAME_TIMEOUT;
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_BLOCK_TIMEOUT, &paramS64);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set block timeout on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_buffer_group_get_internal(&decoder->frame_group, MPP_BUFFER_TYPE_ION);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to retrieve buffer group (code = %d)\n", ret);
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    ret = decoder->mpi->control(decoder->ctx, MPP_DEC_SET_EXT_BUF_GROUP, decoder->frame_group);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to assign buffer group (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_buffer_group_limit_config(decoder->frame_group, 0, FRAMEGROUP_MAX_FRAMES);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set buffer group limit (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    decoder->first_packet = 1;

    av_log(avctx, AV_LOG_DEBUG, "RKMPP decoder initialized successfully.\n");
    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP decoder.\n");
    ffrkmpp_close_decoder(avctx);
    return ret;
}

static int ffrkmpp_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MPP_RET ret = MPP_NOK;

    // handle EOF
    if (!avpkt->size) {
        av_log(avctx, AV_LOG_DEBUG, "End of stream.\n");
        decoder->eos_reached = 1;
        ret = ffrkmpp_write_data(avctx, NULL, 0, 0);
        if (ret)
            av_log(avctx, AV_LOG_ERROR, "Failed to send EOS to decoder (code = %d)\n", ret);
        return ret;
    }

    // on first packet, send extradata
    if (decoder->first_packet) {
        if (avctx->extradata_size) {
            ret = ffrkmpp_write_data(avctx, avctx->extradata,
                                            avctx->extradata_size,
                                            avpkt->pts);
            if (ret) {
                av_log(avctx, AV_LOG_ERROR, "Failed to write extradata to decoder (code = %d)\n", ret);
                return ret;
            }
        }
        decoder->first_packet = 0;
    }

    // now send packet
    ret = ffrkmpp_write_data(avctx, avpkt->data, avpkt->size, avpkt->pts);
    if (ret)
        av_log(avctx, AV_LOG_ERROR, "Failed to write data to decoder (code = %d)\n", ret);

    return ret;
}

static void ffrkmpp_release_frame(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
    AVBufferRef *framecontextref = (AVBufferRef *)opaque;
    RKMPPFrameContext *framecontext = (RKMPPFrameContext *)framecontextref->data;

    mpp_frame_deinit(&framecontext->frame);
    av_buffer_unref(&framecontext->decoder_ref);
    av_buffer_unref(&framecontextref);

    av_free(desc);
}

static int ffrkmpp_retrieve_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    RKMPPFrameContext *framecontext = NULL;
    AVBufferRef *framecontextref = NULL;
    MPP_RET ret = MPP_NOK;
    MppFrame mppframe = NULL;
    MppBuffer buffer = NULL;
    AVDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    int retrycount = 0;

    // on start of decoding, MPP can return -1, which is supposed to be expected
    // this is due to some internal MPP init which is not completed, that will
    // only happen in the first few frames queries, but should not be interpreted
    // as an error, Therefore we need to retry a couple times when we get -1
    // in order to let it time to complete it's init, then we sleep a bit between retries.
retry_get_frame:
    ret = decoder->mpi->decode_get_frame(decoder->ctx, &mppframe);
    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        if (retrycount < 5) {
            av_log(avctx, AV_LOG_DEBUG, "Failed to get a frame, retrying (code = %d, retrycount = %d)\n", ret, retrycount);
            usleep(10000);
            retrycount++;
            goto retry_get_frame;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Failed to get a frame from MPP (code = %d)\n", ret);
            goto fail;
        }
    }

    if (mppframe) {
        // Check wether we have a special frame or not
        if (mpp_frame_get_info_change(mppframe)) {
            av_log(avctx, AV_LOG_INFO, "Decoder noticed an info change (%dx%d), format=%d\n",
                                        (int)mpp_frame_get_width(mppframe), (int)mpp_frame_get_height(mppframe),
                                        (int)mpp_frame_get_fmt(mppframe));

            avctx->width = mpp_frame_get_width(mppframe);
            avctx->height = mpp_frame_get_height(mppframe);

            decoder->mpi->control(decoder->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            decoder->first_frame = 1;

            // here decoder is fully initialized, we need to feed it again with data
            ret = AVERROR(EAGAIN);
            goto fail;
        } else if (mpp_frame_get_eos(mppframe)) {
            av_log(avctx, AV_LOG_DEBUG, "Received a EOS frame.\n");
            decoder->eos_reached = 1;
            ret = AVERROR_EOF;
            goto fail;
        } else if (mpp_frame_get_discard(mppframe)) {
            av_log(avctx, AV_LOG_DEBUG, "Received a discard frame.\n");
            ret = AVERROR(EAGAIN);
            goto fail;
        } else if (mpp_frame_get_errinfo(mppframe)) {
            av_log(avctx, AV_LOG_DEBUG, "Received a errinfo frame.\n");
            ret = AVERROR_EXIT;
            goto fail;
        }

        // here we should have a valid frame
        av_log(avctx, AV_LOG_DEBUG, "Received a frame.\n");

        // setup general frame fields
        frame->format   = AV_PIX_FMT_DRM_PRIME;
        frame->width    = mpp_frame_get_width(mppframe);
        frame->height   = mpp_frame_get_height(mppframe);
        frame->pts      = mpp_frame_get_pts(mppframe);

        // now setup the frame buffer info
        buffer = mpp_frame_get_buffer(mppframe);
        if (buffer) {
            desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
            if (!desc) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            desc->nb_objects = 1;
            desc->objects[0].fd = mpp_buffer_get_fd(buffer);
            desc->objects[0].size = sizeof(AVDRMObjectDescriptor);

            desc->nb_layers = 1;
            layer = &desc->layers[0];
            layer->format = ffrkmpp_get_frameformat(mpp_frame_get_fmt(mppframe));
            layer->nb_planes = 2;

            layer->planes[0].object_index = 0;
            layer->planes[0].offset = 0;
            layer->planes[0].pitch = mpp_frame_get_hor_stride(mppframe);

            layer->planes[1].object_index = 0;
            layer->planes[1].offset = layer->planes[0].pitch * mpp_frame_get_ver_stride(mppframe);
            layer->planes[1].pitch = layer->planes[0].pitch;

            // we also allocate a struct in buf[0] that will allow to hold additionnal information
            // for releasing properly MPP frames and decoder
            framecontextref = av_buffer_allocz(sizeof(*framecontext));
            if (!framecontextref) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            // MPP decoder needs to be closed only when all frames have been released.
            framecontext = (RKMPPFrameContext *)framecontextref->data;
            framecontext->decoder_ref = av_buffer_ref(rk_context->decoder_ref);
            framecontext->frame = mppframe;

            frame->data[0]  = (uint8_t *)desc;
            frame->buf[0]   = av_buffer_create((uint8_t *)desc, sizeof(*desc), ffrkmpp_release_frame,
                                               framecontextref, AV_BUFFER_FLAG_READONLY);

            if (!frame->buf[0]) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            decoder->first_frame = 0;
            return 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Failed to retrieve the frame buffer, frame is dropped (code = %d)\n", ret);
            mpp_frame_deinit(&mppframe);
        }
    } else if (decoder->eos_reached) {
        return AVERROR_EOF;
    } else if (ret == MPP_ERR_TIMEOUT) {
        av_log(avctx, AV_LOG_DEBUG, "Timeout when trying to get a frame from MPP\n");
    }

    return AVERROR(EAGAIN);

fail:
    if (mppframe)
        mpp_frame_deinit(&mppframe);

    if (framecontext)
        av_buffer_unref(&framecontext->decoder_ref);

    if (framecontextref)
        av_buffer_unref(&framecontextref);

    if (desc)
        av_free(desc);

    return ret;
}

static int ffrkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MPP_RET ret = MPP_NOK;
    AVPacket pkt = {0};
    RK_S32 freeslots;

    if (!decoder->eos_reached) {
        // we get the available slots in decoder
        ret = decoder->mpi->control(decoder->ctx, MPP_DEC_GET_STREAM_COUNT, &freeslots);
        if (ret != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get decoder free slots (code = %d).\n", ret);
            return ret;
        }

        if (freeslots > 0) {
            ret = ff_decode_get_packet(avctx, &pkt);
            if (ret < 0 && ret != AVERROR_EOF) {
                return ret;
            }

            ret = ffrkmpp_send_packet(avctx, &pkt);
            av_packet_unref(&pkt);

            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to send packet to decoder (code = %d)\n", ret);
                return ret;
            }
        }

        // make sure we keep decoder full
        if (freeslots > 1 && decoder->first_frame)
            return AVERROR(EAGAIN);
    }

    return ffrkmpp_retrieve_frame(avctx, frame);
}

static void ffrkmpp_flush(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MPP_RET ret = MPP_NOK;

    av_log(avctx, AV_LOG_DEBUG, "Flush.\n");

    ret = decoder->mpi->reset(decoder->ctx);
    if (ret == MPP_OK) {
        decoder->first_frame = 1;
        decoder->first_packet = 1;
    } else
        av_log(avctx, AV_LOG_ERROR, "Failed to reset MPI (code = %d)\n", ret);
}

#define FFRKMPP_DEC_HWACCEL(NAME, ID) \
  AVHWAccel ff_##NAME##_rkmpp_hwaccel = { \
      .name     = #NAME "_rkmpp", \
      .type     = AVMEDIA_TYPE_VIDEO,\
      .id       = ID, \
      .pix_fmt  = AV_PIX_FMT_DRM_PRIME,\
  };

#define FFRKMPP_DEC_CLASS(NAME) \
    static const AVClass ffrkmpp_##NAME##_dec_class = { \
        .class_name = "rkmpp_" #NAME "_dec", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define FFRKMPP_DEC(NAME, ID, BSFS) \
    FFRKMPP_DEC_CLASS(NAME) \
    FFRKMPP_DEC_HWACCEL(NAME, ID) \
    AVCodec ff_##NAME##_rkmpp_decoder = { \
        .name           = #NAME "_rkmpp", \
        .long_name      = NULL_IF_CONFIG_SMALL(#NAME " (rkmpp)"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = ID, \
        .priv_data_size = sizeof(RKMPPDecodeContext), \
        .init           = ffrkmpp_init_decoder, \
        .close          = ffrkmpp_close_decoder, \
        .receive_frame  = ffrkmpp_receive_frame, \
        .flush          = ffrkmpp_flush, \
        .priv_class     = &ffrkmpp_##NAME##_dec_class, \
        .capabilities   = AV_CODEC_CAP_DELAY, \
        .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS, \
        .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_NONE}, \
        .bsfs           = #BSFS, \
    };

FFRKMPP_DEC(h264, AV_CODEC_ID_H264, h264_mp4toannexb)
FFRKMPP_DEC(hevc, AV_CODEC_ID_HEVC, hevc_mp4toannexb)
FFRKMPP_DEC(vp8,  AV_CODEC_ID_VP8,  )
