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

// ==================== 状态转换函数 ====================
static const char* get_sender_state_name(SenderState state) {
    switch (state) {
        case SENDER_STATE_IDLE:                 return "SENDER_STATE_IDLE";
        case SENDER_STATE_JOINING_ROOM:         return "SENDER_STATE_JOINING_ROOM";
        case SENDER_STATE_WAITING_FOR_RECEIVER: return "SENDER_STATE_WAITING_FOR_RECEIVER";
        case SENDER_STATE_SIGNALING:            return "SENDER_STATE_SIGNALING";
        case SENDER_STATE_P2P_CONNECTING:       return "SENDER_STATE_P2P_CONNECTING";
        case SENDER_STATE_DATACHANNEL_READY:    return "SENDER_STATE_DATACHANNEL_READY";
        case SENDER_STATE_PROBING:              return "SENDER_STATE_PROBING";
        case SENDER_STATE_WAITING_STREAM_REQUEST:return "SENDER_STATE_WAITING_STREAM_REQUEST";
        case SENDER_STATE_CREATING_TRACKS:      return "SENDER_STATE_CREATING_TRACKS";
        case SENDER_STATE_STREAMING:            return "SENDER_STATE_STREAMING";
        case SENDER_STATE_ERROR:                return "SENDER_STATE_ERROR";
        default:                                return "SENDER_STATE_UNKNOWN";
    }
}

// ==================== 前向声明 ====================
static void set_sender_state(P2PRtpMuxContext* ctx, SenderState new_state);
static SenderState get_state_thread_safe(P2PRtpMuxContext* ctx);
static int p2p_rtp_mux_signal_handler(P2PContext* ctx, const P2PSignalMessage* msg);
static int p2p_join_room_as_sender(P2PContext* p2p_ctx, const char* room_id);


static void on_pc_connected_callback(P2PContext* ctx, PeerConnectionNode* node) {
    P2PRtpMuxContext* mux_ctx = (P2PRtpMuxContext*)ctx->avctx->priv_data;
    //
    // av_log(ctx->avctx, AV_LOG_INFO, "P2P connection established with sender %s, starting network probing\n", node->remote_id);
    //
    // probe_data_init(&node->network_quality);
    //
    set_sender_state(mux_ctx, SENDER_STATE_DATACHANNEL_READY);

    av_log(ctx->avctx, AV_LOG_INFO, "Sender state set to DATACHANNEL_READY, will send probe request to %s\n", node->remote_id);
}


// ==================== 主要接口函数 ====================

static int p2p_rtp_muxer_init(AVFormatContext* avctx) {
    P2PRtpMuxContext* ctx = (P2PRtpMuxContext*)avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;
    int ret;

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
    ret = p2p_set_signal_message_handler(p2p_ctx, p2p_rtp_mux_signal_handler, on_pc_connected_callback, ctx);
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
    int timeout_sec = ctx->wait_timeout ? ctx->wait_timeout : P2P_WAITING_RECEIVER_TIMEOUT_SEC; // 默认30秒超时
    struct timespec timeout_ts;
    clock_gettime(CLOCK_REALTIME, &timeout_ts);
    timeout_ts.tv_sec += timeout_sec;

    while (get_state_thread_safe(ctx) != SENDER_STATE_STREAMING) {
        int cond_ret = pthread_cond_timedwait(&ctx->state_cond, &ctx->state_mutex, &timeout_ts);

        if (cond_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&ctx->state_mutex);
            av_log(avctx, AV_LOG_ERROR, "Timeout waiting for receiver selection\n");
            return AVERROR(ETIMEDOUT);
        }

        // 检查错误状态
        if (ctx->current_state == SENDER_STATE_ERROR) {
            pthread_mutex_unlock(&ctx->state_mutex);
            av_log(avctx, AV_LOG_ERROR, "Sender entered error state\n");
            return AVERROR_EXTERNAL;
        }

        pthread_mutex_unlock(&ctx->state_mutex);
    }
    
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
    
    if (ctx->current_state < new_state) {
        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
               "Sender state change: %s -> %s\n", 
               get_sender_state_name(ctx->current_state), get_sender_state_name(new_state));
        ctx->current_state = new_state;
        ctx->state_enter_time = av_gettime_relative();
        
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
    char* remote_id = msg->sender_id;
    int ret;
    
    av_log(ctx->avctx, AV_LOG_DEBUG, "Mux processing signal message type: %d from: %s\n", 
           msg->type, remote_id ? remote_id : "unknown");
    
    switch (msg->type) {
        case P2P_MSG_ROOM_JOINED:
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
            {
                const char* target_id = msg->payload.connect_request.target_id;
                av_log(ctx->avctx, AV_LOG_INFO, "Received connect request to %s\n", target_id);

                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, target_id);
                if (node == NULL) {
                    int ret = create_peer_connection(ctx, target_id);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create peer connection for %s\n", target_id);
                        return ret;
                    }

                    node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, target_id);
                }

                // 一次性创建probe、video、audio track

                // 2. 创建视频和音频track
                for (int i = 0; i < ctx->avctx->nb_streams; ++i) {
                    AVStream* stream = ctx->avctx->streams[i];
                    const AVCodecParameters* codecpar = stream->codecpar;
                    int track_id;

                    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !node->video_track) {
                        PeerConnectionTrack* video_track = av_mallocz(sizeof(PeerConnectionTrack));
                        video_track->avctx = node->avctx;
                        if (!video_track) {
                            av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate video track\n");
                            return AVERROR(ENOMEM);
                        }
                        video_track->stream_index = i;

                        /*
        ret = ff_rtp_chain_mux_open(&track->rtp_ctx, avctx, stream, track->rtp_url_context, RTP_MAX_PACKET_SIZE, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_rtp_chain_mux_open failed\n");
            goto fail;
        }
        rtp_mux_ctx = (const RTPMuxContext*)ctx->data_channel.tracks[i].rtp_ctx->priv_data;
                         */
                        if ((track_id = create_send_track(ctx->avctx, stream, node, video_track, i)) < 0) {
                            av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create video track for stream %d, target %s\n", i, target_id);
                            av_freep(&video_track);
                            return track_id;
                        }

                        if ((ret = init_send_track_ex(ctx->avctx, stream, node, video_track, track_id, i)) < 0) {
                            av_log(ctx->avctx, AV_LOG_ERROR, "Failed to initialize video track for stream %d, target %s\n", i, target_id);
                            av_freep(&video_track);
                            return ret;
                        }

                        node->video_track = video_track;
                        av_log(ctx->avctx, AV_LOG_INFO, "Created and initialized video track for receiver %s\n", target_id);

                    } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !node->audio_track) {
                        PeerConnectionTrack* audio_track = av_mallocz(sizeof(PeerConnectionTrack));
                        audio_track->avctx = node->avctx;
                        if (!audio_track) {
                            av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate audio track\n");
                            return AVERROR(ENOMEM);
                        }
                        audio_track->stream_index = i;

                        if ((track_id = create_send_track(ctx->avctx, stream, node, audio_track, i)) < 0) {
                            av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create video track for stream %d, target %s\n", i, target_id);
                            av_freep(&audio_track);
                            return track_id;
                        }

                        if ((ret = init_send_track_ex(ctx->avctx, stream, node, audio_track, track_id, i)) < 0) {
                            av_log(ctx->avctx, AV_LOG_ERROR, "Failed to initialize video track for stream %d, target %s\n", i, target_id);
                            av_freep(&audio_track);
                            return ret;
                        }

                        node->audio_track = audio_track;
                        av_log(ctx->avctx, AV_LOG_INFO, "Created and initialized audio track for receiver %s\n", target_id);
                    }
                }

                // 1. 创建并完全初始化probe track（探测轨道）
                if (!node->probe_track) {
                    AVFormatContext* avctx = ctx->avctx;

                    PeerConnectionTrack* probe_track = av_mallocz(sizeof(PeerConnectionTrack));
                    if (!probe_track) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate probe track\n");
                        return AVERROR(ENOMEM);
                    }

                    ret = init_probe_track_ex(ctx->avctx, node, probe_track);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to initialize probe track for %s\n", target_id);
                        av_freep(&probe_track);
                        return ret;
                    }

                    node->probe_track = probe_track;
                    av_log(ctx->avctx, AV_LOG_INFO, "Created and initialized probe track for receiver %s\n", target_id);
                }

                av_log(ctx->avctx, AV_LOG_INFO, "All track placeholders created for %s (will be fully initialized after SDP negotiation)\n", target_id);
                // node->inited = 1;
                init_peer_connection(ctx, node->pc_id);

                send_local_description(ctx, node, "offer");

                if (ret < 0) {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Failed to send offer message\n");
                }
                set_sender_state(mux_ctx, SENDER_STATE_SIGNALING);
            }
            break;
            
        case P2P_MSG_OFFER:
            {
                int ret;
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node == NULL) {
                    ret = create_peer_connection(ctx, remote_id);//init_peer_connection(ctx, remote_id);
                    if (ret == 0) {
                        node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                        av_log(ctx->avctx, AV_LOG_INFO, "Created new peer connection for receiver %s\n", remote_id);
                    } else {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create peer connection for %s\n", remote_id);
                        return ret;
                    }
                }
                
                if (node && msg->payload.offer.description) {
                    av_log(ctx->avctx, AV_LOG_DEBUG, "Processing Offer SDP from %s, sdp: %s\n", remote_id, msg->payload.offer.description);
                    rtcSetRemoteDescription(node->pc_id, msg->payload.offer.description, "offer");
                    
                    av_log(ctx->avctx, AV_LOG_INFO, "Offer SDP processed for %s\n", remote_id);
                    set_sender_state(mux_ctx, SENDER_STATE_SIGNALING);
                }
                init_peer_connection(ctx, node->pc_id);
            }
            break;
            
        case P2P_MSG_ANSWER:
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node && msg->payload.answer.description) {
                    rtcSetRemoteDescription(node->pc_id, msg->payload.answer.description, "answer");
                } else {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Failed to find peer connection node for %s\n", remote_id);
                    return AVERROR(EINVAL);
                }
            }
            break;
            
        case P2P_MSG_CANDIDATE:
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node && msg->payload.candidate.candidate && msg->payload.candidate.mid) {
                    rtcAddRemoteCandidate(node->pc_id, msg->payload.candidate.candidate, msg->payload.candidate.mid);
                } else {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Failed to find peer connection node for %s\n", remote_id);
                    return AVERROR(EINVAL);
                }
            }
            break;
            
        case P2P_MSG_PROBE_REQUEST:
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Received probe request from %s, probe_id=%s, expected_packets=%d, timeout=%dms\n", 
                           remote_id, msg->payload.probe_request.probe_id, 
                           msg->payload.probe_request.expected_packets, msg->payload.probe_request.timeout_ms);
                    
                    if (msg->payload.probe_request.expected_packets > 0) {
                        node->network_quality.expected_packets = msg->payload.probe_request.expected_packets;
                        av_log(ctx->avctx, AV_LOG_INFO, "Set expected packets to %d for node %s\n",
                               msg->payload.probe_request.expected_packets, remote_id);
                    }
                    
                    // 设置探测超时时间到network_quality中
                    if (msg->payload.probe_request.timeout_ms > 0) {
                        node->network_quality.timeout_ms = msg->payload.probe_request.timeout_ms;
                        node->network_quality.probe_start_time = av_gettime_relative(); // 记录探测开始时间
                        av_log(ctx->avctx, AV_LOG_INFO, "Set probe timeout to %dms for node %s\n",
                               msg->payload.probe_request.timeout_ms, remote_id);
                    }
                    
                    set_sender_state(mux_ctx, SENDER_STATE_PROBING);
                    
                    // 发送探测数据包
                    int ret = send_probe_packets(node);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to send probe packets to %s\n", remote_id);
                    }
                } else {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Failed to find peer connection node for %s\n", remote_id);
                    return AVERROR(EINVAL);
                }
            }
            break;
            
        case P2P_MSG_STREAM_REQUEST:
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Received stream request from %s\n", remote_id);
                    
                    // 检查探测是否完成
                    if (node->status != Connected) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Probe not completed for receiver %s\n", remote_id);
                        return AVERROR(EINVAL);
                    }

                    // 检查tracks是否已经准备好（应该在CONNECT_REQUEST阶段已经创建）
                    if (!node->probe_track || (!node->video_track && !node->audio_track)) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Tracks not ready for receiver %s\n", remote_id);
                        return AVERROR(EINVAL);
                    }
                    
                    av_log(ctx->avctx, AV_LOG_INFO, "All tracks already initialized, ready for streaming to %s\n", remote_id);
                    
                    // 发送推流就绪消息
                    P2PSignalMessage* ready_msg = p2p_create_signal_message(P2P_MSG_STREAM_READY, ctx->local_id, msg->sender_id);
                    if (ready_msg) {
                        ready_msg->payload.stream_ready.stream_id = strdup("main_stream");
                        p2p_send_signal_message(ctx, ready_msg);
                        p2p_free_signal_message(ready_msg);
                    }
                    
                    set_sender_state(mux_ctx, SENDER_STATE_STREAMING);
                    ctx->selected_node = node;
                    node->status = Selected;
                    
                    av_log(ctx->avctx, AV_LOG_INFO, "Stream request processed, now streaming to %s\n", remote_id);
                } else {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Failed to find peer connection node for %s\n", remote_id);
                    return AVERROR(EINVAL);
                }
            }
            break;
            
        case P2P_MSG_ERROR:
            {
                av_log(ctx->avctx, AV_LOG_ERROR, "Received error from %s: code=%s, msg=%s\n",
                       remote_id ? remote_id : "unknown",
                       msg->payload.error.error_code ? msg->payload.error.error_code : "unknown",
                       msg->payload.error.error_msg ? msg->payload.error.error_msg : "unknown");
                set_sender_state(mux_ctx, SENDER_STATE_ERROR);
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
    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_JOIN_ROOM, p2p_ctx->local_id, NULL);
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
    { "wait_timeout", "Timeout for waiting receivers (seconds)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, { .i64 = P2P_WAITING_RECEIVER_TIMEOUT_SEC }, 5, 300, ENC },
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