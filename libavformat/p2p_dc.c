//
// Created by Shaw on 2025/3/4.
//
#include "p2p_dc.h"

#include <libavutil/avstring.h>

#include "p2p_pc.h"
#include "rtpenc.h"
#include "webrtc.h"
#include "rtpenc.h"
#include "rtpenc_chain.h"
#include "rtsp.h"

// 创建rtc track，并赋值回调函数
int init_track_ex(AVFormatContext* avctx,
                  AVStream* stream,
                  PeerConnectionNode* const node,
                  PeerConnectionTrack* const track,
                  int index) {
    if (node == NULL || track == NULL || avctx == NULL || stream == NULL) {
        return AVERROR_INVALIDDATA;
    }
    int ret;
    int track_id = 0;
    const AVCodecParameters* codecpar;
    const RTPMuxContext* rtp_mux_ctx;
    char media_stream_id[37] = { 0 };
    char sdp_stream[SDP_MAX_SIZE] = { 0 };
    char* fmtp;
    rtcTrackInit track_init = { 0 };


    // FFmpeg protocol
    ret = p2p_rtp_init_urlcontext(node, track);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "p2p_rtp_init_urlcontext failed\n");
        goto fail;
    }

    // FFmpeg RTP AVFmtContext
    ret = ff_rtp_chain_mux_open(&track->rtp_ctx, avctx, stream, track->rtp_url_context, RTP_MAX_PACKET_SIZE, index);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "ff_rtp_chain_mux_open failed\n");
        goto fail;
    }
    rtp_mux_ctx = (const RTPMuxContext*)track->rtp_ctx->priv_data;

    // Libdatachannel track_init
    ret = webrtc_generate_media_stream_id(media_stream_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        goto fail;
    }

    track_init.direction = RTC_DIRECTION_SENDONLY;
    track_init.payloadType = rtp_mux_ctx->payload_type;
    track_init.ssrc = rtp_mux_ctx->ssrc;
    track_init.mid = av_asprintf("send, ssrc-%d", track_init.ssrc);
    track_init.name = LIBAVFORMAT_IDENT;
    track_init.msid = media_stream_id;
    track_init.trackId = av_asprintf("%s-video-%d", media_stream_id, index);

    ret = webrtc_convert_codec(stream->codecpar->codec_id, &track_init.codec);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to convert codec\n");
        goto fail;
    }

    // 生成 SDP，并解析其中的 fmtp 参数，然后写到track_init.profile结构中。
    ret = ff_sdp_write_media(sdp_stream, sizeof(sdp_stream), stream, index, NULL, NULL, 0, 0, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to write sdp\n");
        goto fail;
    }
    fmtp = strstr(sdp_stream, "a=fmtp:");
    if (fmtp) {
        track_init.profile = av_strndup(fmtp + 10, strchr(fmtp, '\r') - fmtp - 10);
        track_init.profile = av_asprintf("%s;level-asymmetry-allowed=1", track_init.profile);
        memset(sdp_stream, 0, sizeof(sdp_stream));
    }

    // Libdatachannel track allocate
    if ((track_id = rtcAddTrackEx(node->pc_id, &track_init)) <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create Track\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    track->avctx = node->avctx;
    track->track_id = track_id;
    append_peer_connection_track_to_list(&node->track_caches, track);

    // Libdatachannel common callback
    rtcSetUserPointer(track_id, node);
    ret = rtcSetOpenCallback(track_id, on_track_open_callback);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to rtcSetOpenCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetErrorCallback(track_id, on_track_error_callback);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to rtcSetErrorCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetClosedCallback(track_id, on_track_close_callback);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to rtcSetClosedCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetMessageCallback(track_id, on_track_message_callback);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to rtcSetMessageCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    return 0;
fail:
    if (track_id > 0) {
        rtcDeleteTrack(track_id);
    }
    return ret;
}

//deprecated
int init_track(PeerConnectionNode* const node, char* remote_id) {
    abort();
    if (node == NULL) {
        return AVERROR_INVALIDDATA;
    }

    int track_id = rtcCreateDataChannel(node->pc_id, node->remote_id);

    PeerConnectionTrack* track = av_mallocz(sizeof(PeerConnectionTrack));
    track->avctx = node->avctx;
    track->track_id = track_id;
    append_peer_connection_track_to_list(&node->track_caches, track);

    // common
    rtcSetUserPointer(track_id, node);
    rtcSetOpenCallback(track_id, on_track_open_callback);
    rtcSetErrorCallback(track_id, on_track_error_callback);
    rtcSetClosedCallback(track_id, on_track_close_callback);
    rtcSetMessageCallback(track_id, on_track_message_callback);

    // FFmpeg protocol
    // p2p_rtp_init_urlcontext(node, track);

    return 0;
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
}
