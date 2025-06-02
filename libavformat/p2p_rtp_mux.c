//
// Created by Shaw on 2025/3/10.
//
#include <libavutil/opt.h>

#include "p2p.h"
#include "p2p_pc.h"
#include "p2p_dc.h"
#include "p2p_ws.h"
#include "avformat.h"
#include "mux.h"
#include "p2p_pc.h"

typedef struct P2PRtpMuxContext {
    AVClass *av_class;
    P2PContext p2p_context;
} P2PRtpMuxContext;



static void p2p_rtp_muxer_deinit(AVFormatContext* avctx)
{
    //xy:TODO:

}

static int p2p_rtp_muxer_init(AVFormatContext* avctx)
{
    //入参的avctx是哪个层的ctx？应该是outputfile的ctx
    //xy:Todo:这里的WHIPContext是谁给分配好的？
    // WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;

    P2PRtpMuxContext* ctx = (P2PRtpMuxContext*)avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;
    p2p_ctx->avctx = avctx;

    int i, ret;
    //xy:TODO: 如下参数陆续按需添加
    // char media_stream_id[37] = { 0 }; // 媒体流 ID（UUID 格式）
    const char* local_id = strdup("send");    //xy:TODO: id应该有一套生成规则，可以根据STUN拿到本地的IP，然后生成一个

    ret = p2p_init_connection(p2p_ctx);
    if(ret < 0)
    {
        abort();
    }

    return 0;
}


// 此时初始化n个peer_connection，由服务器返回的n个id，建立起n个连接。
static int p2p_peer_connection_init()
{
    return 0;

}

// 从n个peer_connection中选择一个网络条件最合适的peer，策略函数
static int choose_best_peer()
{
    return 0;

}

//创建n个track
static int p2p_channels_init()
{
    return 0;

}


static int p2p_rtp_muxer_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    switch (codec_id)
    {
    case AV_CODEC_ID_OPUS:
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_PCM_MULAW:
    case AV_CODEC_ID_PCM_ALAW:
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_AV1:
    case AV_CODEC_ID_VP9:
        return 1;
    default:
        return 0;
    }
}

static int p2p_rtp_muxer_check_bitstream(AVFormatContext *s, AVStream *st, const AVPacket *pkt)
{
    //xy:Todo: 根据RTP流的结构检查有效
    // if (st->codecpar->extradata_size && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    //     return ff_stream_add_bitstream_filter(st, "dump_extra", "freq=keyframe");
    return 1;
}

static int p2p_rtp_muxer_write_packet(AVFormatContext* avctx, AVPacket* pkt)
{
    return 0;

}
static int p2p_rtp_muxer_write_header(AVFormatContext* avctx)
{
    return 0;

}
static int p2p_rtp_muxer_write_trailer(AVFormatContext* avctx)
{
    return 0;

}
// 10. WHIP 选项定义
// #define OFFSET(x) offsetof(WHIPContext, x)
// #define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    // WEBRTC_OPTIONS(ENC, OFFSET(data_channel)),
    { NULL },
};


static const AVClass p2p_rtp_muxer_class = {
    .class_name = "P2P RTP muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_p2p_rtp_muxer = {
    .p.name             = "p2pmuxer",
    .p.long_name        = NULL_IF_CONFIG_SMALL("P2P RTP muxer"),
    .p.audio_codec      = AV_CODEC_ID_OPUS, // supported by major browsers
    .p.video_codec      = AV_CODEC_ID_H264,
    .p.flags            = AVFMT_NOFILE | AVFMT_GLOBALHEADER | AVFMT_EXPERIMENTAL,
    .p.priv_class       = &p2p_rtp_muxer_class,
    .priv_data_size     = sizeof(P2PRtpMuxContext),
    .write_packet       = p2p_rtp_muxer_write_packet,
    .write_header       = p2p_rtp_muxer_write_header,
    .write_trailer      = p2p_rtp_muxer_write_trailer,
    .init               = p2p_rtp_muxer_init,
    .deinit             = p2p_rtp_muxer_deinit,
    .query_codec        = p2p_rtp_muxer_query_codec,
    .check_bitstream    = p2p_rtp_muxer_check_bitstream,
};