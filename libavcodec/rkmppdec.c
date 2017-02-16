#include <libdrm/drm_fourcc.h>
#include <rockchip/mpp_buffer.h>
#include <pthread.h>
#include <rockchip/rk_mpi.h>


#include "avcodec.h"
#include "internal.h"
#include "drmprime.h"
#include "libavutil/log.h"
#include "libavutil/common.h"
#include "libavutil/buffer.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"


typedef struct {

    AVBufferRef *ref;

    MppCtx ctx;
    MppApi *mpi;

    char first_packet;
    char eos_reached;

} RKMPPDecoder;

typedef struct {
    AVClass *av_class;

    // bitstream filter in case we need some annexb compliant stream
    AVBSFContext *bsf;

    RKMPPDecoder *decoder;

} RKMPPDecodeContext;

static pthread_mutex_t release_lock = PTHREAD_MUTEX_INITIALIZER;

static MppCodingType ffrkmpp_get_codingtype(AVCodecContext *avctx)
{
    switch(avctx->codec_id) {
        case AV_CODEC_ID_H264:
        return MPP_VIDEO_CodingAVC;

        case AV_CODEC_ID_HEVC:
        return MPP_VIDEO_CodingHEVC;

        case AV_CODEC_ID_VP8:
        return MPP_VIDEO_CodingVP8;

        default:
        return MPP_VIDEO_CodingUnused;
    }
}

static int ffrkmpp_get_frameformat(MppFrameFormat mppformat)
{
    switch(mppformat) {
        case MPP_FMT_YUV420SP:
        return DRM_FORMAT_NV12;

        case MPP_FMT_YUV420SP_10BIT:
        return DRM_FORMAT_NV12_10;

        default:
        return 0;
    }
}

static int ffrkmpp_init_bitstream(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    int ret = 0;

    if (!rk_context->bsf) {
        const AVBitStreamFilter *bsf;

        // check if we need a bitstream filter
        switch(avctx->codec_id) {
            case AV_CODEC_ID_H264:
            bsf = av_bsf_get_by_name("h264_mp4toannexb");
            break;

            case AV_CODEC_ID_HEVC:
            bsf = av_bsf_get_by_name("hevc_mp4toannexb");
            break;

            default:
            av_log(avctx, AV_LOG_DEBUG, "Not using any bitstream filter\n");
            return 0;
        }

        if(!bsf)
            return AVERROR_BSF_NOT_FOUND;

        av_log(avctx, AV_LOG_DEBUG, "using bitstream filter %s\n", bsf->name);

        if ((ret = av_bsf_alloc(bsf, &rk_context->bsf)))
            return ret;

        if (((ret = avcodec_parameters_from_context(rk_context->bsf->par_in, avctx)) < 0) ||
            ((ret = av_bsf_init(rk_context->bsf)) < 0)) {
            av_bsf_free(&rk_context->bsf);
            return ret;
        }
    }

    return 0;
}

static int ffrkmpp_write_data(AVCodecContext *avctx, char *buffer, int size, int64_t pts)
{
    MppPacket packet;
    MPP_RET ret = MPP_OK;
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;

    // create the MPP packet
    ret = mpp_packet_init(&packet, buffer, size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return ret;
    }

    // set the pts
    mpp_packet_set_pts(packet, pts);

    // write it to decoder
    ret = decoder->mpi->decode_put_packet(decoder->ctx, packet);

    mpp_packet_deinit(&packet);

    return (ret != MPP_OK);
}

static int ffrkmpp_close_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;

    av_buffer_unref(&decoder->ref);

    return 0;
}

static void ffrkmpp_release_decoder(void *opaque, uint8_t *data)
{
    RKMPPDecoder *decoder = (RKMPPDecoder *)data;

    if (decoder) {
        mpp_destroy(decoder->ctx);
    }

}

static int ffrkmpp_init_decoder(AVCodecContext *avctx)
{
    MPP_RET ret = MPP_OK;
    MppCodingType codectype = MPP_VIDEO_CodingUnused;

    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder;

    decoder = av_mallocz(sizeof(RKMPPDecoder));
    if (!decoder) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    rk_context->decoder = decoder;

    decoder->ref = av_buffer_create((uint8_t*)decoder, sizeof(*decoder), ffrkmpp_release_decoder,
                                    NULL, AV_BUFFER_FLAG_READONLY);
    if (!decoder->ref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP decoder.\n");

    // Create the MPP context
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        goto fail;
    }

    // initialize the context
    codectype = ffrkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", ret);
        goto fail;
    }

    // initialize mpp
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        goto fail;
    }

    // eventually create a bistream filter for formats that require it
    ret = ffrkmpp_init_bitstream(avctx);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP bitstream.\n");
        goto fail;
    }

    decoder->first_packet = 1;
    decoder->eos_reached = 0;

    av_log(avctx, AV_LOG_DEBUG, "RKMPP decoder initialized successfully.\n");
        return 0;

fail:
    if ((decoder) && (decoder->ref))
        av_buffer_unref(&decoder->ref);

    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP decoder.\n");
    ffrkmpp_close_decoder(avctx);
    return ret;
}

static int ffrkmpp_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;
    AVPacket filter_pkt = {0};
    AVPacket filtered_packet = {0};
    MPP_RET  ret = MPP_OK;
    int retcode = 0;

    // handle EOF
    if (avpkt == NULL) {
        decoder->eos_reached = 1;
        return 0;
    }

    // first we bitstream the packet if it's required
    // this seems to be required for H264 / HEVC / H265
    if (rk_context->bsf) {
        // if we use a bitstream filter, then use it
        if ((ret = av_packet_ref(&filter_pkt, avpkt)) < 0)
            return ret;

        if ((ret = av_bsf_send_packet(rk_context->bsf, &filter_pkt)) < 0) {
            av_packet_unref(&filter_pkt);
            return ret;
        }

        if ((ret = av_bsf_receive_packet(rk_context->bsf, &filtered_packet)) < 0)
            return ret;

        avpkt = &filtered_packet;
    }

    av_log(avctx, AV_LOG_DEBUG, "Sending packet for (%d x %d) with size %d bytes.\n",
                              avctx->width, avctx->height, avpkt->size);


    // on first packet, send extradata
    if (decoder->first_packet) {

        if (rk_context->bsf)
            ret = ffrkmpp_write_data(avctx, rk_context->bsf->par_out->extradata,
                                          rk_context->bsf->par_out->extradata_size,
                                          avpkt->pts);
        else
            ret = ffrkmpp_write_data(avctx, avctx->extradata,
                                          avctx->extradata_size,
                                          avpkt->pts);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Failed to write extradata to decoder\n");
            goto fail;
        }

        decoder->first_packet = 0;
    }

    // now send packet
    ret = ffrkmpp_write_data(avctx, avpkt->data, avpkt->size, avpkt->pts);
    if (ret)
        retcode = AVERROR(EAGAIN);

    // release the ref created by filtered packet
    if (rk_context->bsf)
        av_packet_unref(&filtered_packet);

    return retcode;

fail:
    // release the ref created by filtered packet
    if (rk_context->bsf)
        av_packet_unref(&filtered_packet);

    return ret;
}


static void ffrkmpp_release_frame(void *opaque, uint8_t *data)
{
    MppFrame mppframe = (MppFrame)opaque;

    pthread_mutex_lock(&release_lock);

    if (mppframe)
        mpp_frame_deinit(&mppframe);

    pthread_mutex_unlock(&release_lock);
}

static int ffrkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;
    MPP_RET  ret = MPP_OK;
    MppFrame mppframe = NULL;
    MppBuffer buffer = NULL;
    MppBufferInfo bufferinfo;
    av_drmprime *primedata = NULL;

    // now we will try to get a frame back
    ret = decoder->mpi->decode_get_frame(decoder->ctx, &mppframe);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get a frame from MPP (code = %d)\n", ret);
        goto fail;
    }

    if (mppframe) {
        if (mpp_frame_get_info_change(mppframe)) {
            av_log(avctx, AV_LOG_INFO, "Frame noticed an info change\n");
            decoder->mpi->control(decoder->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&mppframe);
        }
        else {
            // setup general frame fields
            frame->format = AV_PIX_FMT_RKMPP;
            frame->width  = mpp_frame_get_width(mppframe);
            frame->height = mpp_frame_get_height(mppframe);
            frame->pts    = mpp_frame_get_pts(mppframe);


            // now setup the frame buffer info
            buffer = mpp_frame_get_buffer(mppframe);
            if (buffer) {
                ret = mpp_buffer_info_get(buffer, &bufferinfo);
                if (ret != MPP_OK) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to get info from MPP buffer (code = %d)\n", ret);
                    goto fail;
                }

                av_log(avctx, AV_LOG_INFO, "Got a frame ! (%dx%d) pts =%" PRId64 ", format=%d (%d), fd =%d\n",
                   frame->width, frame->height, frame->pts, frame->format, AV_PIX_FMT_RKMPP, mpp_buffer_get_fd(buffer));

                primedata = av_mallocz(sizeof(av_drmprime));
                if (!primedata) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to allocated drm prime data.\n");
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }

                // now fill up the DRMPRIME data
                primedata->strides[0]   = mpp_frame_get_hor_stride(mppframe);
                primedata->strides[1]   = primedata->strides[0];
                primedata->offsets[0]   = 0;
                primedata->offsets[1]   = primedata->strides[0] * mpp_frame_get_ver_stride(mppframe);
                primedata->fd           = mpp_buffer_get_fd(buffer);
                primedata->format       = ffrkmpp_get_frameformat(mpp_frame_get_fmt(mppframe));

                frame->data[3] = (uint8_t*)primedata;
                frame->buf[0]  = av_buffer_create((uint8_t*)primedata, sizeof(*primedata), ffrkmpp_release_frame,
                                              mppframe, AV_BUFFER_FLAG_READONLY);

                if (!frame->buf[0]) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }

                // add a ref to decoder for each frame
                frame->buf[1] = av_buffer_ref(decoder->ref);

                return 0;
            }
        }
    }
    else
    {
        if (decoder->eos_reached)
            return AVERROR_EOF;
    }

    return AVERROR(EAGAIN);

fail:
    if (primedata)
        av_free(primedata);

    if (mppframe)
        mpp_frame_deinit(&mppframe);

    return ret;
}

static void ffrkmpp_flush(AVCodecContext *avctx)
{
    MPP_RET ret = MPP_OK;
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;

    ret = decoder->mpi->reset(decoder->ctx);
    if (ret == MPP_OK)
        decoder->first_packet = 1;
    else
        av_log(avctx, AV_LOG_ERROR, "Failed to reset MPI (code = %d)\n", ret);
}

#define FFRKMPP_DEC_HWACCEL(NAME, ID) \
  AVHWAccel ff_##NAME##_rkmpp_hwaccel = { \
      .name       = #NAME "_rkmpp", \
      .type       = AVMEDIA_TYPE_VIDEO,\
      .id         = ID, \
      .pix_fmt    = AV_PIX_FMT_RKMPP,\
  };

#define FFRKMPP_DEC_CLASS(NAME) \
    static const AVClass ffrkmpp_##NAME##_dec_class = { \
        .class_name = "rkmpp_" #NAME "_dec", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define FFRKMPP_DEC(NAME, ID) \
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
        .send_packet    = ffrkmpp_send_packet, \
        .receive_frame  = ffrkmpp_receive_frame, \
        .flush          = ffrkmpp_flush, \
        .priv_class     = &ffrkmpp_##NAME##_dec_class, \
        .capabilities   = AV_CODEC_CAP_DELAY, \
        .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS, \
        .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_RKMPP, \
                                                         AV_PIX_FMT_NONE}, \
    };

FFRKMPP_DEC(h264, AV_CODEC_ID_H264)
FFRKMPP_DEC(hevc, AV_CODEC_ID_HEVC)
FFRKMPP_DEC(vp8,  AV_CODEC_ID_VP8)
