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
#include "rtpenc.h"
#include "rtpenc_chain.h"
#include "rtsp.h"
#include "version.h"
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
    

    // 配置参数
    char* room_id;              // 可配置的房间ID
    int wait_timeout;           // 等待超时时间（秒）
} P2PRtpMuxContext;

// ==================== 前向声明 ====================
static void set_sender_state(P2PRtpMuxContext* ctx, SenderState new_state);
static SenderState get_state_thread_safe(P2PRtpMuxContext* ctx);
static int p2p_rtp_mux_signal_handler(P2PContext* ctx, const P2PSignalMessage* msg);
static int p2p_join_room_as_sender(P2PContext* p2p_ctx, const char* room_id);


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
    av_log(avctx, AV_LOG_INFO, "Waiting for receivers to join room %s...\n", ctx->room_id ? ctx->room_id : "default_room");
    
    // 4. 使用条件变量等待拉流端连接并选择本节点
    int timeout_sec = ctx->wait_timeout ? ctx->wait_timeout : 30; // 默认30秒超时
    struct timespec timeout_ts;
    clock_gettime(CLOCK_REALTIME, &timeout_ts);
    timeout_ts.tv_sec += timeout_sec;

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
    
    if (ctx->p2p_context.room_info.room_id) {
        av_freep(&ctx->p2p_context.room_info.room_id);
    }
    if (ctx->p2p_context.room_info.room_name) {
        av_freep(&ctx->p2p_context.room_info.room_name);
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
                
                if (msg->payload.room_joined.room_id) {
                    if (ctx->room_info.room_id) {
                        av_freep(&ctx->room_info.room_id);
                    }
                    ctx->room_info.room_id = strdup(msg->payload.room_joined.room_id);
                }
                
                if (msg->payload.room_joined.local_id) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Server assigned local_id: %s\n", 
                           msg->payload.room_joined.local_id);
                    
                    if (ctx->local_id) {
                        av_freep(&ctx->local_id);
                    }
                    ctx->local_id = strdup(msg->payload.room_joined.local_id);
                }
                
                av_log(ctx->avctx, AV_LOG_INFO, "Room joined successfully, role: %s\n",
                       msg->payload.room_joined.role ? msg->payload.room_joined.role : "unknown");
                
                set_sender_state(mux_ctx, SENDER_STATE_WAITING_FOR_RECEIVER);
            }
            break;
            
        case P2P_MSG_CONNECT_REQUEST:
            // 收到连接请求，主动发起连接
            {
                const char* target_id = msg->payload.connect_request.target_id;
                av_log(ctx->avctx, AV_LOG_INFO, "Received connect request to %s\n", target_id);

                // 创建PeerConnection并发送Offer
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, target_id);
                if (node == NULL) {
                    int ret = init_peer_connection(ctx, target_id);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create peer connection for %s\n", target_id);
                        return ret;
                    }

                    node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, target_id);
                }

                // 如果node还没有创建tracks，先创建它们（包括完整的RTP context初始化）
                //xy: 一次性完成所有track的创建和初始化，避免多次往返，提升效率
                if (!node->probe_track) {
                    AVFormatContext* avctx = ctx->avctx;
                    
                    // 1. 创建并完全初始化probe track（探测轨道）
                    PeerConnectionTrack* probe_track = av_mallocz(sizeof(PeerConnectionTrack));
                    if (!probe_track) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate probe track\n");
                        return AVERROR(ENOMEM);
                    }
                    
                    int ret = init_probe_track_ex(ctx->avctx, node, probe_track);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to initialize probe track for %s\n", target_id);
                        av_freep(&probe_track);
                        return ret;
                    }
                    
                    node->probe_track = probe_track;
                    av_log(ctx->avctx, AV_LOG_INFO, "Created and initialized probe track for %s\n", target_id);
                    
                    // 2. 为每个媒体流创建并完全初始化Track（包括RTP上下文）
                    for (int i = 0; i < avctx->nb_streams; ++i) {
                        AVStream* stream = avctx->streams[i];
                        const AVCodecParameters* codecpar = stream->codecpar;
                        
                        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !node->video_track) {
                            // 创建并初始化视频轨道
                            PeerConnectionTrack* video_track = av_mallocz(sizeof(PeerConnectionTrack));
                            if (!video_track) {
                                av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate video track\n");
                                return AVERROR(ENOMEM);
                            }
                            
                            video_track->stream_index = i;
                            video_track->track_type = PeerConnectionTrackType_Video;
                            
                            // 立即初始化RTP上下文，避免延迟
                            ret = init_send_track_ex(avctx, stream, node, video_track, i);
                            if (ret < 0) {
                                av_log(ctx->avctx, AV_LOG_ERROR, "Failed to initialize video track for stream %d, target %s\n", i, target_id);
                                av_freep(&video_track);
                                return ret;
                            }
                            
                            node->video_track = video_track;
                            av_log(ctx->avctx, AV_LOG_INFO, "Created and initialized video track for stream %d, target %s\n", i, target_id);
                            
                        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !node->audio_track) {
                            // 创建并初始化音频轨道
                            PeerConnectionTrack* audio_track = av_mallocz(sizeof(PeerConnectionTrack));
                            if (!audio_track) {
                                av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate audio track\n");
                                return AVERROR(ENOMEM);
                            }
                            
                            audio_track->stream_index = i;
                            audio_track->track_type = PeerConnectionTrackType_Audio;
                            
                            // 立即初始化RTP上下文，避免延迟
                            ret = init_send_track_ex(avctx, stream, node, audio_track, i);
                            if (ret < 0) {
                                av_log(ctx->avctx, AV_LOG_ERROR, "Failed to initialize audio track for stream %d, target %s\n", i, target_id);
                                av_freep(&audio_track);
                                return ret;
                            }
                            
                            node->audio_track = audio_track;
                            av_log(ctx->avctx, AV_LOG_INFO, "Created and initialized audio track for stream %d, target %s\n", i, target_id);
                        }
                    }
                    
                    av_log(ctx->avctx, AV_LOG_INFO, "All tracks created and initialized for %s\n", target_id);
                }
                
                if (!node->probe_track || !node->probe_track->track_id) {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Probe track not found for %s\n", target_id);
                    return AVERROR(EINVAL);
                }

                // 获取SDP
                int sdp_size = 4096; // 预设一个足够大的缓冲区大小
                char* sdp = av_malloc(sdp_size);
                if (sdp) {
                    int ret = rtcGetTrackDescription(node->probe_track->track_id, sdp, sdp_size);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "rtcGetTrackDescription失败，track_id: %d\n", node->probe_track->track_id);
                        av_free(sdp);
                        sdp = NULL;
                    }
                } else {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate memory for SDP\n");
                    return AVERROR(ENOMEM);
                }

                // 发送信令服务器
                P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_OFFER, target_id); 
                msg->payload.offer.description = sdp;
                p2p_send_signal_message(ctx, msg);
                p2p_free_signal_message(msg);

                set_sender_state(mux_ctx, SENDER_STATE_SIGNALING);
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
                    
                    // 检查tracks是否已经准备好（应该在CONNECT_REQUEST阶段已经创建）
                    if (!node->probe_track || !node->video_track || !node->audio_track) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Tracks not ready for receiver %s\n", msg->id);
                        return AVERROR(EINVAL);
                    }
                    
                    av_log(ctx->avctx, AV_LOG_INFO, "All tracks already initialized, ready for streaming to %s\n", msg->id);
                    
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
                    
                    av_log(ctx->avctx, AV_LOG_INFO, "Stream request processed, now streaming to %s\n", msg->id);
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