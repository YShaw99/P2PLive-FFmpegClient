//
// Created by Shaw on 2025/3/4.
//

// Todo: 代码封装不够整洁，后续优化

#include "p2p_dc.h"

#include "avformat.h"
#include "internal.h"
#include "avio_internal.h"
#include "p2p_pc.h"
#include "rtpenc.h"
#include "webrtc.h"
#include "rtpenc_chain.h"
#include "rtsp.h"
#include "version.h"
#include "internal.h"
#include "libavutil/avstring.h"

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
    track_init->direction    = RTC_DIRECTION_SENDONLY;
    track_init->payloadType  = rtp_mux_ctx->payload_type;
    track_init->ssrc         = rtp_mux_ctx->ssrc;
    track_init->mid          = av_asprintf("send-ssrc-%d", track_init->ssrc);
    track_init->name         = LIBAVFORMAT_IDENT;
    track_init->msid         = av_strdup(media_stream_id);
    track_init->trackId      = av_asprintf("%s-send-%d", media_stream_id, index);
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
    track_init->direction    = RTC_DIRECTION_RECVONLY;
    track_init->payloadType  = payload_type;
    track_init->ssrc         = ssrc;
    track_init->mid          = av_asprintf("recv-ssrc-%d", track_init->ssrc);
    track_init->name         = LIBAVFORMAT_IDENT;
    track_init->msid         = av_strdup(media_stream_id);
    track_init->trackId      = av_asprintf("%s-recv-%d", media_stream_id, index);
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
    if ((fmtp = strstr(sdp_stream, "a=fmtp:"))) {
        const char* fmtp_start = fmtp + 7; // 跳过 "a=fmtp:"
        const char* fmtp_end = strchr(fmtp_start, '\r');
        if (!fmtp_end) fmtp_end = strchr(fmtp_start, '\n');
        if (fmtp_end) {
            track_init->profile = av_strndup(fmtp_start, fmtp_end - fmtp_start);
            if (track_init->profile) {
                char* new_profile = av_asprintf("%s;level-asymmetry-allowed=1", track_init->profile);
                av_freep(&track_init->profile);
                track_init->profile = new_profile;
            }
        }
    }

    if ((track_id = rtcAddTrackEx(node->pc_id, track_init)) <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create Track\n");
        return AVERROR_EXTERNAL;
    }

    track->avctx = node->avctx;
    track->track_id = track_id;
    append_peer_connection_track_to_list(&node->track_caches, track);

    // 设置回调函数
    rtcSetUserPointer(track_id, node);
    if ((ret = rtcSetOpenCallback(track_id, on_track_open_callback)) < 0 ||
        (ret = rtcSetErrorCallback(track_id, on_track_error_callback)) < 0 ||
        (ret = rtcSetClosedCallback(track_id, on_track_close_callback)) < 0
        // 设置rtcSetMessageCallback后无法通过rtcReceive主动获取。
        // || (ret = rtcSetMessageCallback(track_id, on_track_message_callback)) < 0
    ) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set track callback\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int setup_probe_common_logic(AVFormatContext* avctx,
                                    PeerConnectionNode* node,
                                    PeerConnectionTrack* track)
{
    int dc_id, ret;

    rtcDataChannelInit dc_init = {0};
    dc_init.reliability.unordered = true;
    dc_init.reliability.unreliable = true;
    dc_init.reliability.maxPacketLifeTime = 1000;

    if ((dc_id = rtcCreateDataChannelEx(node->pc_id, PROBE_CHANNEL_LABEL, &dc_init)) <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create probe DataChannel\n");
        return AVERROR_EXTERNAL;
    }

    // 初始化轨道公共属性
    track->avctx = node->avctx;
    track->track_id = dc_id;
    track->track_type = PeerConnectionTrackType_ProbeChannel;
    append_peer_connection_track_to_list(&node->track_caches, track);

    // 设置回调函数
    rtcSetUserPointer(dc_id, node);
    if ((ret = rtcSetOpenCallback(dc_id, on_track_open_callback)) < 0 ||
        (ret = rtcSetErrorCallback(dc_id, on_track_error_callback)) < 0 ||
        (ret = rtcSetClosedCallback(dc_id, on_track_close_callback)) < 0
        // || (ret = rtcSetMessageCallback(dc_id, on_track_message_callback)) < 0
        ) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set datachannel callback\n");
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
                       int index)
{
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

    // 2. 接收端无需手动初始化track，通过发送端的sdp自动协商。
    // if ((ret = setup_recv_track(avctx, stream->codecpar->codec_id, track, index, payload_type, ssrc, &track_init)) < 0) {
    //     goto fail;
    // }
    //xy:TODO: 应该需要删掉？
    if ((ret = setup_common_logic(avctx, stream, node, track, &track_init)) < 0) {
        goto fail;
    }

    // 3. 分配 RTP 上下文，根据avctx信息装填上下文
    track->rtp_ctx = avformat_alloc_context();
    if (!track->rtp_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    track->rtp_ctx->max_delay = avctx->max_delay;
    track->rtp_ctx->interrupt_callback = avctx->interrupt_callback;

    // 4. 初始化 SDP 文件的 IO 上下文
    char sdp_track[SDP_MAX_SIZE] = {0};
    ret = rtcGetTrackDescription(track->track_id, sdp_track, sizeof(sdp_track));
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get track description\n");
        goto fail;
    }
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

    // 6. 打开文件描述符
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
    if (track->rtp_ctx) {
        avformat_close_input(&track->rtp_ctx);
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
    const AVCodecParameters* codecpar = stream->codecpar;
    int ret;

    // 设置轨道类型和配置PTS信息
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            avpriv_set_pts_info(stream, 32, 1, 90000); // 视频使用90kHz时钟
            track->track_type = PeerConnectionTrackType_Video;
            node->video_track = track;
            av_log(avctx, AV_LOG_INFO, "Initializing video track for stream %d\n", index);
            break;
        case AVMEDIA_TYPE_AUDIO:
            // 检查音频参数
            if (codecpar->sample_rate != 48000) {
                av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate %d. Only 48kHz is supported\n", codecpar->sample_rate);
                return AVERROR(EINVAL);
            }
            avpriv_set_pts_info(stream, 32, 1, codecpar->sample_rate); // 音频使用采样率作为时钟
            track->track_type = PeerConnectionTrackType_Audio;
            node->audio_track = track;
            av_log(avctx, AV_LOG_INFO, "Initializing audio track for stream %d\n", index);
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unsupported codec type for stream %d\n", index);
            return AVERROR(EINVAL);
    }

    // 设置流索引
    track->stream_index = index;

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
    if ((ret = setup_send_track(avctx, stream, track, index, &track_init)) < 0) {
        goto fail;
    }

    // 调用公共逻辑
    if ((ret = setup_common_logic(avctx, stream, node, track, &track_init)) < 0) {
        goto fail;
    }

    av_log(avctx, AV_LOG_INFO, "Successfully initialized %s track for stream %d\n",
           codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio", index);

    return 0;

fail:
    if (track->track_id > 0) {
        rtcDeleteTrack(track->track_id);
    }
    if (track->rtp_ctx) {
        avformat_close_input(&track->rtp_ctx);
    }
    return ret;
}

int init_probe_track_ex(AVFormatContext* avctx,
                        PeerConnectionNode* const node,
                        PeerConnectionTrack* const track)
{
    int ret;
    if (node == NULL || track == NULL || avctx == NULL) {
        return AVERROR_INVALIDDATA;
    }
    av_log(avctx, AV_LOG_INFO, "Initializing probe datachannel\n");

    track->track_type = PeerConnectionTrackType_ProbeChannel;
    track->stream_index = -1;

    // 调用探测通道公共逻辑
    if ((ret = setup_probe_common_logic(avctx, node, track)) < 0) {
        goto fail;
    }

    // 设置为探测轨道
    node->probe_track = track;

    av_log(avctx, AV_LOG_INFO, "Successfully initialized probe datachannel\n");

    return 0;

fail:
    if (track->track_id > 0) {
        rtcDeleteDataChannel(track->track_id);
    }
    return ret;
}

// -------- dc callback --------
void on_track_open_callback(int track_id, void* ptr)
{
    printf("[FFmpegP2P][Track] on_track_open | track: %d\n", track_id);
    PeerConnectionNode* node = ptr;
    if (find_peer_connection_track_by_track_id(node->track_caches, track_id) == NULL) {
        return;
    }

    // 获取轨道信息用于调试
    char buffer1[1024];
    char buffer2[1024];
    int size1 = 1024;
    int size2 = 1024;
    int ret;
    rtcDirection direction;
    
    ret = rtcGetTrackDescription(track_id, buffer1, size1);
    if (ret > 0) size1 = ret;

    ret = rtcGetTrackMid(track_id, buffer2, size2);
    if (ret > 0) size2 = ret;

    ret = rtcGetTrackDirection(track_id, &direction);
    
    av_log(node->avctx, AV_LOG_INFO, "Track %d opened, direction: %d\n", track_id, direction);
}

void on_track_close_callback(int track_id, void* ptr) {
    printf("[FFmpegP2P][Track] close | track: %d\n", track_id);
    PeerConnectionNode* node = ptr;
    if (node && node->avctx) {
        av_log(node->avctx, AV_LOG_INFO, "Track %d closed\n", track_id);
    }
}

void on_track_error_callback(int track_id, const char *error, void *ptr) {
    printf("[FFmpegP2P][Track] error | error: %s, track: %d\n", error, track_id);
    PeerConnectionNode* node = ptr;
    if (node && node->avctx) {
        av_log(node->avctx, AV_LOG_ERROR, "Track %d error: %s\n", track_id, error);
    }
}

void on_track_message_callback(int track_id, const char *message, int size, void *ptr) {
    printf("[FFmpegP2P][Track] message! track: %d, message size: %d\n", track_id, size);
    
    PeerConnectionNode *node = ptr;
    if (!node) return;

    // 检查是否是探测数据包
    if (size == sizeof(ProbePacket)) {
        // handle_probe_packet(node, message, size); // 需要实现探测包处理函数
        av_log(node->avctx, AV_LOG_DEBUG, "Received probe packet on track %d\n", track_id);
    } else {
        av_log(node->avctx, AV_LOG_DEBUG, "Received message on track %d, size: %d\n", track_id, size);
    }
}
