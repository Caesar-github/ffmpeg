/*
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "mem.h"
#include "pixdesc.h"
#include "pixfmt.h"

typedef struct RKMPPFramesContext {
    int dummy;
} RKMPPFramesContext;

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
};

static int rkmpp_frames_init(AVHWFramesContext *ctx)
{
    printf("rkmpp_frames_init called\n");
    // CUDAFramesContext *priv = ctx->internal->priv;
    // int i;

    // for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        // if (ctx->sw_format == supported_formats[i])
            // break;
    // }
    // if (i == FF_ARRAY_ELEMS(supported_formats)) {
        // av_log(ctx, AV_LOG_ERROR, "Pixel format '%s' is not supported\n",
               // av_get_pix_fmt_name(ctx->sw_format));
        // return AVERROR(ENOSYS);
    // }

    // av_pix_fmt_get_chroma_sub_sample(ctx->sw_format, &priv->shift_width, &priv->shift_height);

    // if (!ctx->pool) {
        // int size;

        // switch (ctx->sw_format) {
        // case AV_PIX_FMT_NV12:
        // case AV_PIX_FMT_YUV420P:
            // size = ctx->width * ctx->height * 3 / 2;
            // break;
        // case AV_PIX_FMT_YUV444P:
            // size = ctx->width * ctx->height * 3;
            // break;
        // }

        // ctx->internal->pool_internal = av_buffer_pool_init2(size, ctx, cuda_pool_alloc, NULL);
        // if (!ctx->internal->pool_internal)
            // return AVERROR(ENOMEM);
    // }

    return 0;
}

static int rkmpp_get_buffer(AVHWFramesContext *ctx, AVFrame *frame)
{
    printf("rkmpp_get_buffer called\n");
    // frame->buf[0] = av_buffer_pool_get(ctx->pool);
    // if (!frame->buf[0])
        // return AVERROR(ENOMEM);

    // switch (ctx->sw_format) {
    // case AV_PIX_FMT_NV12:
        // frame->data[0]     = frame->buf[0]->data;
        // frame->data[1]     = frame->data[0] + ctx->width * ctx->height;
        // frame->linesize[0] = ctx->width;
        // frame->linesize[1] = ctx->width;
        // break;
    // case AV_PIX_FMT_YUV420P:
        // frame->data[0]     = frame->buf[0]->data;
        // frame->data[2]     = frame->data[0] + ctx->width * ctx->height;
        // frame->data[1]     = frame->data[2] + ctx->width * ctx->height / 4;
        // frame->linesize[0] = ctx->width;
        // frame->linesize[1] = ctx->width / 2;
        // frame->linesize[2] = ctx->width / 2;
        // break;
    // case AV_PIX_FMT_YUV444P:
        // frame->data[0]     = frame->buf[0]->data;
        // frame->data[1]     = frame->data[0] + ctx->width * ctx->height;
        // frame->data[2]     = frame->data[1] + ctx->width * ctx->height;
        // frame->linesize[0] = ctx->width;
        // frame->linesize[1] = ctx->width;
        // frame->linesize[2] = ctx->width;
        // break;
    // default:
        // av_frame_unref(frame);
        // return AVERROR_BUG;
    // }

    // frame->format = AV_PIX_FMT_CUDA;
    // frame->width  = ctx->width;
    // frame->height = ctx->height;

    return 0;
}

static int rkmpp_transfer_get_formats(AVHWFramesContext *ctx,
                                     enum AVHWFrameTransferDirection dir,
                                     enum AVPixelFormat **formats)
{
    enum AVPixelFormat *fmts;

    printf("rkmpp_transfer_get_formats called\n");

    fmts = av_malloc_array(2, sizeof(*fmts));
    if (!fmts)
        return AVERROR(ENOMEM);

    fmts[0] = AV_PIX_FMT_NV12;
    fmts[1] = AV_PIX_FMT_NONE;

    *formats = fmts;

    return 0;
}

static int rkmpp_transfer_data_from(AVHWFramesContext *ctx, AVFrame *dst,
                                   const AVFrame *src)
{
    printf("rkmpp_transfer_data_from called\n");
    return 0;
}

static int rkmpp_transfer_data_to(AVHWFramesContext *ctx, AVFrame *dst,
                                 const AVFrame *src)
{
    printf("rkmpp_transfer_data_to called\n");
    return 0;
}

const HWContextType ff_hwcontext_type_rkmpp = {
    .type                 = AV_HWDEVICE_TYPE_RKMPP,
    .name                 = "RKMPP",

    //.device_hwctx_size    = sizeof(AVCUDADeviceContext),
    .frames_priv_size     = sizeof(RKMPPFramesContext),

    .frames_init          = rkmpp_frames_init,
    .frames_get_buffer    = rkmpp_get_buffer,
    .transfer_get_formats = rkmpp_transfer_get_formats,
    .transfer_data_to     = rkmpp_transfer_data_to,
    .transfer_data_from   = rkmpp_transfer_data_from,

    .pix_fmts             = (const enum AVPixelFormat[]){ AV_PIX_FMT_RKMPP, AV_PIX_FMT_NONE },
};
