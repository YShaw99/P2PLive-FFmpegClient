//
// Created by Shaw on 2025/3/4.
//

// Todo: 代码封装不够整洁，后续优化

#include "p2p_dc.h"

#include <libavutil/avstring.h>

#include "p2p_pc.h"
#include "rtpenc.h"
#include "webrtc.h"
#include "rtpenc.h"
#include "rtpenc_chain.h"
#include "rtsp.h"
#include "version.h"

static int setup_send_track(AVFormatContext* avctx,
                            AVStream* stream,
                            PeerConnectionTrack* track,
                            int index,
                            rtcTrackInit* track_init)
{
    const RTPMuxContext* rtp_mux_ctx = (const RTPMuxContext*)track->rtp_ctx->priv_data;
    char media_stream_id[37] = {0};
    int ret;

    // 生成媒体流ID
    if ((ret = webrtc_generate_media_stream_id(media_stream_id)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        return ret;
    }

    // 配置轨道唯一参数
    track_init->direction = RTC_DIRECTION_SENDONLY;
    track_init->payloadType = rtp_mux_ctx->payload_type;
    track_init->ssrc = rtp_mux_ctx->ssrc;
    track_init->mid = av_asprintf("send, ssrc-%d", track_init->ssrc);
    track_init->name = LIBAVFORMAT_IDENT;
    track_init->msid = media_stream_id;
    track_init->trackId = av_asprintf("%s-send-%d", media_stream_id, index);
    ret = webrtc_convert_codec(stream->codecpar->codec_id, &track_init->codec);

    return ret;
}

static int setup_recv_track(AVFormatContext* avctx,
                            enum AVCodecID stream_codec_id,
                            PeerConnectionTrack* track,
                            int index,
                            int payload_type,
                            uint32_t ssrc,
                            rtcTrackInit* track_init)
{
    char media_stream_id[37] = {0};
    int ret;

    // 生成媒体流ID
    if ((ret = webrtc_generate_media_stream_id(media_stream_id)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        return ret;
    }

    // 配置轨道唯一参数
    track_init->direction = RTC_DIRECTION_RECVONLY;
    track_init->payloadType = payload_type;
    track_init->ssrc = ssrc;
    track_init->mid = av_asprintf("recv, ssrc-%d", track_init->ssrc);
    track_init->name = LIBAVFORMAT_IDENT;
    track_init->msid = media_stream_id;
    track_init->trackId = av_asprintf("%s-recv-%d", media_stream_id, index);
    ret = webrtc_convert_codec(stream_codec_id, &track_init->codec);

    return ret;
}

/* 公共逻辑封装函数（对应原69-119行） */
static int setup_common_logic(AVFormatContext* avctx,
                            AVStream* stream,
                            PeerConnectionNode* node,
                            PeerConnectionTrack* track,
                            rtcTrackInit* track_init)
{
    char sdp_stream[SDP_MAX_SIZE] = {0};
    char* fmtp = NULL;
    int track_id, ret;

    // 生成SDP描述
    if ((ret = ff_sdp_write_media(sdp_stream, sizeof(sdp_stream),
                                stream, 0, NULL, NULL, 0, 0, NULL)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to write sdp\n");
        return ret;
    }

    // 解析fmtp参数
    // if ((fmtp = strstr(sdp_stream, "a=fmtp:"))) {
    //     track_init->profile = av_strndup(fmtp + 10, strchr(fmtp, '\r') - fmtp - 10);
    //     track_init->profile = av_asprintf("%s;level-asymmetry-allowed=1", track_init->profile);
    // }

    // 创建libdatachannel轨道
    // if ((track_id = rtcAddTrackEx(node->pc_id, track_init)) <= 0) {
    //     av_log(avctx, AV_LOG_ERROR, "Failed to create Track\n");
    //     return AVERROR_EXTERNAL;
    // }

    // 初始化轨道公共属性
    track->avctx = node->avctx;
    // track->track_id = track_id;
    track_id = track->track_id;
    // append_peer_connection_track_to_list(&node->track_caches, track);

    // 设置回调函数
    rtcSetUserPointer(track_id, node);
    if ((ret = rtcSetOpenCallback(track_id, on_track_open_callback)) < 0 ||
        (ret = rtcSetErrorCallback(track_id, on_track_error_callback)) < 0 ||
        (ret = rtcSetClosedCallback(track_id, on_track_close_callback)) < 0
        // ||
        // (ret = rtcSetMessageCallback(track_id, on_track_message_callback)) < 0
        ) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set track callback\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}


int init_recv_track_ex(AVFormatContext* avctx,
                AVStream* stream,
                 PeerConnectionNode* const node,
                 PeerConnectionTrack* const track,
                 int payload_type,
                            uint32_t ssrc,
                 int index) {
    rtcTrackInit track_init = {0};
    AVDictionary* options = NULL;
    const AVInputFormat* infmt;
    FFIOContext sdp_pb;
    int ret;

    // 1. 初始化协议上下文
    if ((ret = p2p_rtp_init_urlcontext(track)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "p2p_rtp_init_urlcontext failed\n");
        goto fail;
    }

    // 2. 装填track属性，初始化Track
    // if ((ret = setup_recv_track(avctx, stream->codecpar->codec_id, track, index, payload_type, ssrc, &track_init)) < 0)
    //     goto fail;

    if ((ret = setup_common_logic(avctx, stream, node, track, &track_init)) < 0)
        goto fail;

    // 3. 分配 RTP 上下文，根据Track信息装填上下文
    track->rtp_ctx = avformat_alloc_context();
    if (!track->rtp_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    track->rtp_ctx->max_delay = avctx->max_delay;
    track->rtp_ctx->interrupt_callback = avctx->interrupt_callback;

    // 4. 初始化 SDP 文件的 IO 上下文
    char sdp_track[SDP_MAX_SIZE] = { 0 };
    ret = rtcGetTrackDescription(track->track_id, sdp_track, sizeof(sdp_track));
    if (ret < 0) abort();
    ffio_init_read_context(&sdp_pb, (uint8_t*)sdp_track, strlen(sdp_track));
    track->rtp_ctx->pb = &sdp_pb.pub;

    // 5. 设置 SDP 选项, 打开 SDP 输入
    av_dict_set(&options, "sdp_flags", "custom_io", 0);
    infmt = av_find_input_format("sdp");
    ret = avformat_open_input(&track->rtp_ctx, "temp.sdp", infmt, &options);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "avformat_open_input failed\n");
        goto fail;
    }

    // 6. 打开文件描述符, 什么作用？
    ret = ffio_fdopen(&track->rtp_ctx->pb, track->rtp_url_context);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "ffio_fdopen failed\n");
        goto fail;
    }

    return 0;

fail:
        if (track->track_id > 0) {
            rtcDeleteTrack(track->track_id);
        }
    return ret;
}

int init_send_track_ex(AVFormatContext* avctx,
                 AVStream* stream,
                 PeerConnectionNode* const node,
                 PeerConnectionTrack* const track,
                 int index)
{
    if (node == NULL || track == NULL || avctx == NULL || stream == NULL) {
        return AVERROR_INVALIDDATA;
    }

    rtcTrackInit track_init = {0};
    int ret;

    // 初始化协议上下文
    if ((ret = p2p_rtp_init_urlcontext(track)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "p2p_rtp_init_urlcontext failed\n");
        goto fail;
    }

    // 打开RTP复用器
    if ((ret = ff_rtp_chain_mux_open(&track->rtp_ctx, avctx, stream,
                                   track->rtp_url_context, RTP_MAX_PACKET_SIZE, index)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "ff_rtp_chain_mux_open failed\n");
        goto fail;
    }

    // 调用独有逻辑
    if ((ret = setup_send_track(avctx, stream, track, index, &track_init)) < 0)
        goto fail;

    // 调用公共逻辑
    if ((ret = setup_common_logic(avctx, stream, node, track, &track_init)) < 0)
        goto fail;

    return 0;

fail:
    if (track->track_id > 0) {
        rtcDeleteTrack(track->track_id);
    }
    return ret;
}

// -------- dc callback --------
void on_track_open_callback(int track_id, void* ptr) {

    printf("[FFmpegP2P][Track] on_track_open | track: %d\n", track_id);
    PeerConnectionNode* node = ptr;
    if(find_peer_connection_track_by_track_id(node->track_caches, track_id) == NULL) {
        return;
    }
    //Todo: 这里没有做datachannel有效性检测，并且size=-1是以字符串发送，而非binary

    int tr = track_id;
    char buffer1[1024];
    char buffer2[1024];
    int size1 = 1024;
    int size2 = 1024;
    int ret;
    rtcDirection direction;
    ret = rtcGetTrackDescription(tr, buffer1, size1);
    size1 = ret;

    ret = rtcGetTrackMid(tr, buffer2, size2);
    size2 = ret;

    ret = rtcGetTrackDirection(tr, &direction);

    ret = rtcSendMessage(track_id, buffer1, size1);
    ret = rtcSendMessage(track_id, buffer1, size1);
    // ret = rtcSendMessage(ctx->track_id, (const char*)buf, size);

    //按照之前的DataChannel，这里发送Hello对方可以接受，更改为Track之后

}

void on_track_close_callback(int track_id, void* ptr) {
    printf("[FFmpegP2P][Track] close | track: %d\n", track_id);
}

void on_track_error_callback(int track_id, const char *error, void *ptr) {
    printf("[FFmpegP2P][Track] error | error: %s, track: %d\n", error, track_id);
    //Todo: 这里可以做一些事
}

void on_track_message_callback(int track_id, const char *message, int size, void *ptr) {
    printf("[FFmpegP2P][Track] message! track: %d, message: %s \n", track_id, message);
    
    // PeerConnectionNode *node = ptr;
    // if (!node) return;
    //
    // // 检查是否是探测数据包
    // if (size == sizeof(ProbePacket)) {
    //     handle_probe_packet(node, message, size);
    // }
}
