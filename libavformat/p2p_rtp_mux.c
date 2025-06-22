//
// Created by Shaw on 2025/3/10.
//
#include <unistd.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>

#include "p2p.h"
#include "p2p_pc.h"
#include "p2p_dc.h"
#include "p2p_ws.h"
#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "p2p_pc.h"
#include "rtsp.h"
#include "webrtc.h"

typedef struct P2PRtpMuxContext {
    AVClass *av_class;
    P2PContext p2p_context;
} P2PRtpMuxContext;



static void p2p_rtp_muxer_deinit(AVFormatContext* avctx) {
    //xy:TODO:

}

static int p2p_rtp_muxer_init(AVFormatContext* avctx) {
    //入参的avctx是哪个层的ctx？应该是outputfile的ctx
    //xy:Todo:这里的WHIPContext是谁给分配好的？
    // WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;

    P2PRtpMuxContext* ctx = (P2PRtpMuxContext*)avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;

    const AVChannelLayout supported_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVStream* stream;
    const AVCodecParameters* codecpar;
    PeerConnectionNode* node;
    PeerConnectionTrack* track;
    int ret;

    p2p_ctx->avctx = avctx;
    char* local_id = strdup("send");    //xy:TODO:dev: id应该有一套生成规则，可以根据STUN拿到本地的IP，然后生成一个
    p2p_ctx->local_id = local_id;

    // 1.连接信令服务器
    if ((ret = p2p_init_signal_server(p2p_ctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to connect signal server\n");
        ret = AVERROR(ENOTCONN);
        goto fail;
    }

    // 2. Nodes
    //xy:ToDo:dev: 2期主动mock 远端的id。3期修改为从服务器分发合适的id
    const char *remote_ids[] = {
        "recv"
        /*, "recv2"*/
    };


    // 3. head Nodes
    for (int i = 0; i < sizeof(remote_ids) / sizeof(remote_ids[0]); ++i) {
        const char* remote_id = remote_ids[i];
        ret = init_peer_connection(p2p_ctx, remote_id);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate peer connection node\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        node = find_peer_connection_node_by_remote_id(p2p_ctx->peer_connection_node_caches, remote_ids[i]);

        // 3.1. every Tracks on node (libdatachannel API & FFmpeg Url Ctx)
        for (int j = 0; j < avctx->nb_streams; ++j) {
            stream = avctx->streams[j];
            codecpar = stream->codecpar;
            track = av_mallocz(sizeof(PeerConnectionTrack));
            if (track == NULL) {
                av_log(avctx, AV_LOG_ERROR, "Failed to allocate peer connection node\n");
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            track->stream_index = j;


            switch (codecpar->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    avpriv_set_pts_info(stream, 32, 1, 90000); // 设置视频时间戳信息
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    if (codecpar->sample_rate != 48000) {
                        av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate. Only 48kHz is supported\n");
                        ret = AVERROR(EINVAL);
                        goto fail;
                    }
                    if (av_channel_layout_compare(&codecpar->ch_layout, &supported_layout) != 0) {
                        av_log(avctx, AV_LOG_ERROR, "Unsupported channel layout. Only stereo is supported\n");
                        ret = AVERROR(EINVAL);
                        goto fail;
                    }
                    avpriv_set_pts_info(stream, 32, 1, codecpar->sample_rate); // 设置音频时间戳信息
                    break;
                default:
                    continue;
            }

            ret = init_send_track_ex(avctx, stream, node, track, j);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }
        }
    }

    return 0;
fail:
    abort();
}

static int p2p_rtp_muxer_write_header(AVFormatContext* avctx) {
    P2PRtpMuxContext*const rtp_mux_context = (P2PRtpMuxContext*const)avctx->priv_data;
    P2PContext*const p2p_context = &rtp_mux_context->p2p_context;

    int ret;
    URLContext* h = NULL;
    char* headers;
    char offer_sdp[SDP_MAX_SIZE] = { 0 };
    char response_sdp[SDP_MAX_SIZE] = { 0 };
    //xy:Todo:optimize: 这是个问题..如何找到当前正在连接的node？当前的逻辑比较鄙陋，write header时将所有node都连接
    PeerConnectionNode* curr;

    // 1. 将所有连接 SetLocalDescription，回调函数中将会自动发送给信令服务器
    curr = p2p_context->peer_connection_node_caches;
    while (curr != NULL) {

        if (ret = rtcSetLocalDescription(curr->pc_id, "offer") != RTC_ERR_SUCCESS) {
            av_log(p2p_context->avctx, AV_LOG_ERROR, "Failed to set local description return %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        curr = curr->next;
    }
    curr = p2p_context->peer_connection_node_caches;
    // sleep(10000);
    while (curr) {
        curr = curr->next ? curr->next : p2p_context->peer_connection_node_caches;
        if (curr->status == Connected) {
            curr->status = Selected;
            p2p_context->selected_node = curr;
            break;
        }
        usleep(100);
    }
    //xy:Todo:optimize:二期先临时选择最快连接成功的peer_connection进行连接，后期再进行网络条件比较和策略选择。



    return 0;
fail:
    abort();
}

static int p2p_rtp_muxer_query_codec(enum AVCodecID codec_id, int std_compliance) {
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

static int p2p_rtp_muxer_check_bitstream(AVFormatContext *s, AVStream *st, const AVPacket *pkt) {
    //xy:Todo: 根据RTP流的结构检查有效
    if (st->codecpar->extradata_size && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return ff_stream_add_bitstream_filter(st, "dump_extra", "freq=keyframe");
    return 1;
}

static int p2p_rtp_muxer_write_packet(AVFormatContext* avctx, AVPacket* pkt) {
    //xy:休息回来继续从这里入手，记录一下已经连接的那个peer，然后开始发送消息
    //排查一下为什么没有调用track on open//已经发送成功，demo已经可以收到128类型的消息，直接写一个ffmpeg recv 让他能够demux并播放
    P2PRtpMuxContext*const rtp_mux_context = (P2PRtpMuxContext*const)avctx->priv_data;
    P2PContext*const p2p_context = &rtp_mux_context->p2p_context;
    PeerConnectionNode* node = p2p_context->selected_node;
    PeerConnectionTrack* track = find_peer_connection_track_by_stream_index(node->track_caches, pkt->stream_index);
    if (track == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot find target track!\n");
        return AVERROR(EINVAL);
    }
    AVFormatContext* rtpctx = track->rtp_ctx;

    // 这里借助了on_state_change
    // if (ctx->data_channel.state != RTC_CONNECTED) {
    //     av_log(avctx, AV_LOG_ERROR, "Connection is not open\n");
    //     return AVERROR(EINVAL);
    // }
    // 5.2 写入 RTP 数据包
    return av_write_frame(rtpctx, pkt);
    // */
    //
    // P2PRtpMuxContext*const rtp_mux_context = (P2PRtpMuxContext*const)avctx->priv_data;
    // P2PContext* p2p_context = &rtp_mux_context->p2p_context;
    // PeerConnectionNode* curr = p2p_context->peer_connection_node_caches;
    // while (curr) {
    //     PeerConnectionTrack* curr_tr = curr->track_caches;
    //     while(curr_tr) {
    //         unsigned char a [1024] = { 0 };
    //         memset(a, 255, 1023);
    //         int ret = rtcSendMessage(curr_tr->track_id, a, 1023);
    //         if (ret != RTC_ERR_SUCCESS) {
    //             abort();
    //         }
    //         curr_tr = curr_tr->next;
    //     }
    //     curr = curr->next;
    //
    // }
    // curr = p2p_context->peer_connection_node_caches;
    //
    // usleep(1000);
    // return av_write_frame(curr->track_caches->rtp_ctx, pkt);
    // return 0;

}
static int p2p_rtp_muxer_write_trailer(AVFormatContext* avctx) {
    //ff在这里只是做了 webrtc close resource
    // 里面调用了HTTP请求
    // av_opt_set(h->priv_data, "method", "DELETE", 0);

    return 0;
}

// 此时初始化n个peer_connection，由服务器返回的n个id，建立起n个连接。
// static int p2p_peer_connection_init()
// {
//     return 0;
// }

//xy:TODO:三期 从n个peer_connection中选择一个网络条件最合适的peer，策略函数
static int choose_best_peer() {
    return 0;
}

//创建n个track
static int p2p_channels_init() {
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