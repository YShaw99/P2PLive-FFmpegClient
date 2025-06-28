//
// Created by Shaw on 2025/3/10.
//
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/log.h>
#include <libavutil/random_seed.h>
#include <libavformat/avformat.h>
#include <libavformat/internal.h>
#include "mux.h"
#include "rtsp.h"
#include "webrtc.h"
#include "p2p.h"
#include "p2p_pc.h"
#include "p2p_dc.h"
#include "p2p_ws.h"
#include "p2p_probe.h"
#include "p2p_signal.h"
#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "rtsp.h"
#include "webrtc.h"
#include "cjson/cJSON.h"


// 新增：推流端状态机
typedef enum SenderState {
    SENDER_STATE_IDLE,                    // 空闲状态
    SENDER_STATE_JOINING_ROOM,            // 加入房间中
    SENDER_STATE_WAITING_FOR_RECEIVER,    // 等待拉流端
    SENDER_STATE_SIGNALING,               // 信令交换中
    SENDER_STATE_P2P_CONNECTING,          // P2P连接建立中
    SENDER_STATE_DATACHANNEL_READY,       // DataChannel就绪
    SENDER_STATE_PROBING,                 // 网络探测中
    SENDER_STATE_WAITING_STREAM_REQUEST,  // 等待推流请求
    SENDER_STATE_CREATING_TRACKS,         // 创建媒体轨道中
    SENDER_STATE_STREAMING,               // 推流中
    SENDER_STATE_ERROR                    // 错误状态
} SenderState;


// typedef struct RoomInfo {
//     char* room_id;
//     int max_receivers;          // 最大拉流端数量
//     int current_receivers;      // 当前拉流端数量
// } RoomInfo;

typedef struct P2PRtpMuxContext {
    AVClass *av_class;
    P2PContext p2p_context;
    
    // 新增状态管理
    SenderState current_state;
    int64_t state_enter_time;
    
    // 状态变化同步机制
    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    
    // 房间信息
    // RoomInfo room_info;
    
    
    // 流信息（延迟初始化）
    int streams_count;
    AVStream** streams_cache;
    int tracks_created;
    
    // 配置参数
    char* room_id;              // 可配置的房间ID
    int wait_timeout;           // 等待超时时间（秒）
} P2PRtpMuxContext;


// ==================== 主要接口函数 ====================

static int p2p_rtp_muxer_init(AVFormatContext* avctx) {
    P2PRtpMuxContext* ctx = (P2PRtpMuxContext*)avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;
    int ret;

    // 初始化同步原语
    if (pthread_mutex_init(&ctx->state_mutex, NULL) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize state mutex\n");
        return AVERROR(ENOMEM);
    }
    
    if (pthread_cond_init(&ctx->state_cond, NULL) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize state condition variable\n");
        pthread_mutex_destroy(&ctx->state_mutex);
        return AVERROR(ENOMEM);
    }

    set_sender_state(ctx, SENDER_STATE_IDLE);
    
    p2p_ctx->avctx = avctx;
    
    // 生成本地ID（推流端）
    // char local_id[64];
    // snprintf(local_id, sizeof(local_id), "sender_%08x", av_get_random_seed());
    // p2p_ctx->local_id = strdup(local_id);
    
    // // 初始化房间信息
    // ctx->room_info.room_id = ctx->room_id ? strdup(ctx->room_id) : strdup("default_room");
    // ctx->room_info.room_name = strdup("Default Room");
    // ctx->room_info.max_receivers = 10;
    // ctx->room_info.current_receivers = 0;
    
    // 缓存流信息，但不立即创建Track
    // ctx->streams_count = avctx->nb_streams;
    // ctx->streams_cache = av_mallocz(sizeof(AVStream*) * ctx->streams_count);
    // for (int i = 0; i < ctx->streams_count; ++i) {
    //     ctx->streams_cache[i] = avctx->streams[i];
    // }
    // ctx->tracks_created = 0;
    
    av_log(avctx, AV_LOG_INFO, "P2P Sender initialized with ID: %s\n", p2p_ctx->local_id);
    
    set_sender_state(ctx, SENDER_STATE_JOINING_ROOM);
    
    // 1. 连接信令服务器
    if ((ret = p2p_init_signal_server(p2p_ctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to connect signal server\n");
        return ret;
    }
    
    // 2. 设置信令消息处理回调
    ret = p2p_set_signal_message_handler(p2p_ctx, p2p_rtp_mux_signal_handler, ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set signal message handler\n");
        return ret;
    }
    
    // 3. 加入房间（作为推流端）
    p2p_join_room_as_sender(p2p_ctx, ctx->room_id);
    av_log(avctx, AV_LOG_INFO, "Joining room as sender: %s\n", ctx->room_id ? ctx->room_id : "default_room");
    
    // 3. 设置状态为等待拉流端，开始阻塞等待
    set_sender_state(ctx, SENDER_STATE_WAITING_FOR_RECEIVER);
    av_log(avctx, AV_LOG_INFO, "Waiting for receivers to join room %s...\n", ctx->room_info.room_id);
    
    // 4. 使用条件变量等待拉流端连接并选择本节点
    int timeout_sec = ctx->wait_timeout ? ctx->wait_timeout : 30; // 默认30秒超时
    struct timespec timeout_ts;
    clock_gettime(CLOCK_REALTIME, &timeout_ts);
    timeout_ts.tv_sec += timeout_sec;
    
    pthread_mutex_lock(&ctx->state_mutex);
    
    while (get_state_thread_safe(ctx) != SENDER_STATE_STREAMING) {
        // 使用条件变量等待状态变化，支持超时
        int cond_ret = pthread_cond_timedwait(&ctx->state_cond, &ctx->state_mutex, &timeout_ts);
        
        if (cond_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&ctx->state_mutex);
            av_log(avctx, AV_LOG_ERROR, "Timeout waiting for receiver selection\n");
            return AVERROR(ETIMEDOUT);
        }
        
        // 检查错误状态
        if (get_state_thread_safe(ctx) == SENDER_STATE_ERROR) {
            pthread_mutex_unlock(&ctx->state_mutex);
            av_log(avctx, AV_LOG_ERROR, "Sender entered error state\n");
            return AVERROR_EXTERNAL;
        }
    }
    
    pthread_mutex_unlock(&ctx->state_mutex);
    
    av_log(avctx, AV_LOG_INFO, "Receiver selected, ready to start streaming\n");
    
    return 0;
}

static int p2p_rtp_muxer_write_header(AVFormatContext* avctx) {
    P2PRtpMuxContext* ctx = (P2PRtpMuxContext*)avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;
    
    if (!p2p_ctx->selected_node) {
        av_log(avctx, AV_LOG_ERROR, "No receiver selected for streaming\n");
        return AVERROR(EINVAL);
    }
    
    // P2P connection has connected. ready to publish video stream now, do nothing. 
    av_log(avctx, AV_LOG_INFO, "Ready to start streaming to receiver %s\n", 
           p2p_ctx->selected_node->remote_id);
    
    return 0;
}

static int p2p_rtp_muxer_write_packet(AVFormatContext* avctx, AVPacket* pkt) {
    P2PRtpMuxContext* ctx = (P2PRtpMuxContext*)avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;
    
    if (get_state_thread_safe(ctx) != SENDER_STATE_STREAMING) {
        av_log(avctx, AV_LOG_ERROR, "Not in streaming state\n");
        return AVERROR(EINVAL);
    }
    
    PeerConnectionNode* node = p2p_ctx->selected_node;
    if (!node) {
        av_log(avctx, AV_LOG_ERROR, "No selected receiver node\n");
        return AVERROR(EINVAL);
    }
    
    PeerConnectionTrack* track = find_peer_connection_track_by_stream_index(
        node->track_caches, pkt->stream_index);
    if (track == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot find target track for stream %d\n", pkt->stream_index);
        return AVERROR(EINVAL);
    }
    
    AVFormatContext* rtpctx = track->rtp_ctx;
    return av_write_frame(rtpctx, pkt);
}

static int p2p_rtp_muxer_write_trailer(AVFormatContext* avctx) {
    P2PRtpMuxContext* ctx = (P2PRtpMuxContext*)avctx->priv_data;
    
    av_log(avctx, AV_LOG_INFO, "Ending P2P streaming session\n");
    
    // 通知拉流端结束
    if (ctx->p2p_context.selected_node) {
        cJSON* message = cJSON_CreateObject();
        cJSON_AddStringToObject(message, "type", "stream_end");
        cJSON_AddStringToObject(message, "sender_id", ctx->p2p_context.local_id);
        
        char* json_str = cJSON_PrintUnformatted(message);
        rtcSendMessage(ctx->p2p_context.web_socket, json_str, strlen(json_str));
        
        free(json_str);
        cJSON_Delete(message);
    }
    
    return 0;
}

static void p2p_rtp_muxer_deinit(AVFormatContext* avctx) {
    P2PRtpMuxContext* ctx = (P2PRtpMuxContext*)avctx->priv_data;
    
    pthread_cond_destroy(&ctx->state_cond);
    pthread_mutex_destroy(&ctx->state_mutex);
    
    if (ctx->room_info.room_id) {
        av_freep(&ctx->room_info.room_id);
    }
    if (ctx->room_info.room_name) {
        av_freep(&ctx->room_info.room_name);
    }
    if (ctx->streams_cache) {
        av_freep(&ctx->streams_cache);
    }
    
    // 关闭P2P连接
    p2p_close_resource(&ctx->p2p_context);
}

static int p2p_rtp_muxer_query_codec(enum AVCodecID codec_id, int std_compliance) {
    switch (codec_id) {
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
    if (st->codecpar->extradata_size && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return ff_stream_add_bitstream_filter(st, "dump_extra", "freq=keyframe");
    return 1;
}

static void set_sender_state(P2PRtpMuxContext* ctx, SenderState new_state) {
    pthread_mutex_lock(&ctx->state_mutex);
    
    if (ctx->current_state != new_state) {
        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
               "Sender state change: %d -> %d\n", ctx->current_state, new_state);
        ctx->current_state = new_state;
        ctx->state_enter_time = av_gettime_relative();
        
        // 通知所有等待状态变化的线程
        pthread_cond_broadcast(&ctx->state_cond);
    }
    
    pthread_mutex_unlock(&ctx->state_mutex);
}

static SenderState get_state_thread_safe(P2PRtpMuxContext* ctx) {
    pthread_mutex_lock(&ctx->state_mutex);
    SenderState state = ctx->current_state;
    pthread_mutex_unlock(&ctx->state_mutex);
    return state;
}


static int p2p_rtp_mux_signal_handler(P2PContext* ctx, const P2PSignalMessage* msg) {
    P2PRtpMuxContext* mux_ctx = (P2PRtpMuxContext*)ctx->avctx->priv_data;
    
    av_log(ctx->avctx, AV_LOG_DEBUG, "Processing signal message type: %d from: %s\n", 
           msg->type, msg->id ? msg->id : "unknown");
    
    switch (msg->type) {
        case P2P_MSG_ROOM_JOINED:
            // 房间加入成功
            {
                av_log(ctx->avctx, AV_LOG_INFO, "Successfully joined room: %s\n", 
                    msg->payload.room_joined.room_id);
                set_sender_state(mux_ctx, SENDER_STATE_WAITING_FOR_RECEIVER);
            }
            break;
            
        case P2P_MSG_OFFER:
            // 收到拉流端的Offer，有拉流端主动发起连接
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                if (node == NULL) {
                    // 新建 PeerConnection
                    int ret = init_peer_connection(ctx, msg->id);
                    if (ret == 0) {
                        node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                        set_sender_state(mux_ctx, SENDER_STATE_SIGNALING);
                    } else {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create peer connection for %s\n", msg->id);
                        return ret;
                    }
                }
                if (node && msg->payload.offer.description) {
                    rtcSetRemoteDescription(node->pc_id, msg->payload.offer.description, "offer");
                }
            }
            break;
            
        case P2P_MSG_ANSWER:
            // 处理Answer
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                if (node && msg->payload.answer.description) {
                    rtcSetRemoteDescription(node->pc_id, msg->payload.answer.description, "answer");
                } else {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Failed to find peer connection node for %s\n", msg->id);
                    return AVERROR(EINVAL);
                }
            }
            break;
            
        case P2P_MSG_CANDIDATE:
            // 处理ICE候选
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                if (node && msg->payload.candidate.candidate && msg->payload.candidate.mid) {
                    rtcAddRemoteCandidate(node->pc_id, msg->payload.candidate.candidate, msg->payload.candidate.mid);
                }
            }
            break;
            
        case P2P_MSG_PROBE_REQUEST:
            // 处理探测请求
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                if (node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Received probe request from %s, probe_id=%s\n", 
                           msg->id, msg->payload.probe_request.probe_id);
                    
                    set_sender_state(mux_ctx, SENDER_STATE_PROBING);
                    
                    // 发送探测数据包
                    int ret = send_probe_packets(node);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to send probe packets to %s\n", msg->id);
                    }
                }
            }
            break;
            
        case P2P_MSG_STREAM_REQUEST:
            // 处理推流请求
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                if (node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Received stream request from %s\n", msg->id);
                    
                    // 检查探测是否完成
                    if (node->status != Connected || node->network_quality.final_score <= 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Probe not completed for receiver %s\n", msg->id);
                        return AVERROR(EINVAL);
                    }
                    
                    set_sender_state(mux_ctx, SENDER_STATE_CREATING_TRACKS);
                    
                    // 创建媒体轨道
                    int ret = create_media_tracks_for_receiver(mux_ctx, node);
                    if (ret < 0) {
                        set_sender_state(mux_ctx, SENDER_STATE_ERROR);
                        return ret;
                    }
                    
                    // 发送推流就绪消息
                    P2PSignalMessage* ready_msg = p2p_create_signal_message(P2P_MSG_STREAM_READY, ctx->local_id);
                    if (ready_msg) {
                        ready_msg->payload.stream_ready.stream_id = strdup("main_stream");
                        p2p_send_signal_message(ctx, ready_msg);
                        p2p_free_signal_message(ready_msg);
                    }
                    
                    set_sender_state(mux_ctx, SENDER_STATE_STREAMING);
                    ctx->selected_node = node;
                    node->status = Selected;
                }
            }
            break;
            
        default:
            av_log(ctx->avctx, AV_LOG_DEBUG, "Unhandled message type: %d\n", msg->type);
            break;
    }
    
    return 0;
}

// ==================== P2P相关回调 ====================

static int p2p_join_room_as_sender(P2PContext* p2p_ctx, const char* room_id) {
    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_JOIN_ROOM, NULL);
    if (!msg) {
        av_log(p2p_ctx->avctx, AV_LOG_ERROR, "无法分配P2PSignalMessage\n");
        return AVERROR(ENOMEM);
    }
    msg->payload.join_room.role = strdup("sender");
    if (room_id)
        msg->payload.join_room.room_id = strdup(room_id);
    else
        msg->payload.join_room.room_id = strdup("default_room");
    // Todo: 后续完善
    // msg->payload.join_room.capabilities = strdup("video:h264;bitrate:4000kbps;framerate:30;audio:aac;abitrate:128kbps");

    int ret = p2p_send_signal_message(p2p_ctx, msg);

    av_log(p2p_ctx->avctx, AV_LOG_INFO, "Joining room as sender: %s\n", room_id);

    p2p_free_signal_message(msg);
    return ret;
}

// ==================== 媒体轨道创建（延迟创建）====================

static int create_media_tracks_for_receiver(P2PRtpMuxContext* ctx, PeerConnectionNode* node) {
    AVFormatContext* avctx = ctx->p2p_context.avctx;
    const AVChannelLayout supported_layout = AV_CHANNEL_LAYOUT_STEREO;
    
    av_log(avctx, AV_LOG_INFO, "Creating media tracks for receiver %s\n", node->remote_id);
    
    // 为每个媒体流创建Track
    for (int i = 0; i < avctx->nb_streams; ++i) {
        AVStream* stream = avctx->streams[i];
        const AVCodecParameters* codecpar = stream->codecpar;
        
        PeerConnectionTrack* track = av_mallocz(sizeof(PeerConnectionTrack));
        if (track == NULL) {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate track\n");
            return AVERROR(ENOMEM);
        }
        
        track->stream_index = i;
        
        switch (codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                avpriv_set_pts_info(stream, 32, 1, 90000);
                track->track_type = PeerConnectionTrackType_Video;
                node->video_track = track;
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (codecpar->sample_rate != 48000) {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate. Only 48kHz is supported\n");
                    av_freep(&track);
                    return AVERROR(EINVAL);
                }
                if (av_channel_layout_compare(&codecpar->ch_layout, &supported_layout) != 0) {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported channel layout. Only stereo is supported\n");
                    av_freep(&track);
                    return AVERROR(EINVAL);
                }
                avpriv_set_pts_info(stream, 32, 1, codecpar->sample_rate);
                track->track_type = PeerConnectionTrackType_Audio;
                node->audio_track = track;
                break;
            default:
                av_freep(&track);
                continue;
        }
        
        // 调用外部函数创建实际的Track
        int ret = init_send_track_ex(avctx, stream, node, track, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to initialize track for stream %d\n", i);
            av_freep(&track);
            return ret;
        }
        
        // 添加到轨道缓存
        append_peer_connection_track_to_list(&node->track_caches, track);
    }
    
    ctx->tracks_created = 1;
    return 0;
}

// ==================== 配置选项 ====================

#define OFFSET(x) offsetof(P2PRtpMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
    { "room_id", "Room ID to join", OFFSET(room_id), AV_OPT_TYPE_STRING, { .str = "default_room" }, 0, 0, ENC },
    { "wait_timeout", "Timeout for waiting receivers (seconds)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, { .i64 = 30 }, 5, 300, ENC },
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
    .p.audio_codec      = AV_CODEC_ID_OPUS,
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