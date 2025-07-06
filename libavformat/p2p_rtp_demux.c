//
// P2P RTP Demuxer
// Created by Shaw on 2025/3/10.
//

#include "libavutil/log.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/random_seed.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavcodec/codec_id.h"
#include "avformat.h"
#include "internal.h"
#include "version.h"
#include "rtsp.h"
#include "avio_internal.h"
#include "url.h"
#include "p2p.h"
#include "p2p_dc.h"
#include "p2p_pc.h"
#include "p2p_probe.h"
#include "p2p_signal.h"
#include "rtc/rtc.h"

// ==================== 拉流端状态枚举 ====================
typedef enum ReceiverState {
    RECEIVER_STATE_IDLE,                    // 空闲状态
    RECEIVER_STATE_JOINING_ROOM,            // 加入房间中
    RECEIVER_STATE_WAITING_FOR_SENDER,      // 等待推流端
    RECEIVER_STATE_SIGNALING,               // 信令交换中
    RECEIVER_STATE_P2P_CONNECTING,          // P2P连接建立中
    RECEIVER_STATE_DATACHANNEL_READY,       // DataChannel就绪
    RECEIVER_STATE_PROBING,                 // 网络探测中
    RECEIVER_STATE_PROBE_COMPLETED,         // 探测完成
    RECEIVER_STATE_REQUESTING_STREAM,       // 请求推流中
    RECEIVER_STATE_STREAMING,               // 接收推流中
    RECEIVER_STATE_ERROR                    // 错误状态
} ReceiverState;

typedef struct P2PRtpDemuxContext {
    AVClass *av_class;
    P2PContext p2p_context;
    
    // 状态管理
    ReceiverState current_state;
    int64_t state_enter_time;
    
    // 配置参数
    char* room_id;              // 房间ID
    int wait_timeout;           // 等待超时时间（秒）
    
    AVStream *streams[2];       // 假设音视频各一个流
} P2PRtpDemuxContext;

// ==================== 状态转换函数 ====================
static const char* get_receiver_state_name(ReceiverState state) {
    switch (state) {
        case RECEIVER_STATE_IDLE: return "RECEIVER_STATE_IDLE";
        case RECEIVER_STATE_JOINING_ROOM: return "RECEIVER_STATE_JOINING_ROOM";
        case RECEIVER_STATE_WAITING_FOR_SENDER: return "RECEIVER_STATE_WAITING_FOR_SENDER";
        case RECEIVER_STATE_SIGNALING: return "RECEIVER_STATE_SIGNALING";
        case RECEIVER_STATE_P2P_CONNECTING: return "RECEIVER_STATE_P2P_CONNECTING";
        case RECEIVER_STATE_DATACHANNEL_READY: return "RECEIVER_STATE_DATACHANNEL_READY";
        case RECEIVER_STATE_PROBING: return "RECEIVER_STATE_PROBING";
        case RECEIVER_STATE_PROBE_COMPLETED: return "RECEIVER_STATE_PROBE_COMPLETED";
        case RECEIVER_STATE_REQUESTING_STREAM: return "RECEIVER_STATE_REQUESTING_STREAM";
        case RECEIVER_STATE_STREAMING: return "RECEIVER_STATE_STREAMING";
        case RECEIVER_STATE_ERROR: return "RECEIVER_STATE_ERROR";
        default: return "RECEIVER_STATE_UNKNOWN";
    }
}

// ==================== 前向声明 ====================
static void set_receiver_state(P2PRtpDemuxContext* ctx, ReceiverState new_state);
static int p2p_rtp_demux_signal_handler(P2PContext* ctx, const P2PSignalMessage* msg);
static int p2p_join_room_as_receiver(P2PContext* p2p_ctx, const char* room_id);
static int handle_datachannel_ready(P2PRtpDemuxContext* ctx, PeerConnectionNode* node);
static void on_datachannel_established(P2PRtpDemuxContext* ctx, PeerConnectionNode* node);
static int check_state_timeout(P2PRtpDemuxContext* ctx, int64_t timeout_us);
static int pending_for_probing_node(P2PRtpDemuxContext* ctx);
static void on_pc_connected_callback(P2PContext* ctx, PeerConnectionNode* node);

// ==================== 主要初始化函数 ====================
static int p2p_rtp_demux_init(AVFormatContext *avctx) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;
    int ret;

    // 初始化状态
    p2p_ctx->avctx = avctx;
    set_receiver_state(ctx, RECEIVER_STATE_JOINING_ROOM);

    // 1. 连接信令服务器
    if ((ret = p2p_init_signal_server(p2p_ctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init signal server\n");
        return ret;
    }

    // 2.1. 设置信令消息处理回调
    ret = p2p_set_signal_message_handler(p2p_ctx, p2p_rtp_demux_signal_handler, on_pc_connected_callback, ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set signal message handler\n");
        return ret;
    }
    
    // 2.2. 设置P2P连接建立成功的回调（用于触发网络探测）
    av_log(avctx, AV_LOG_INFO, "Set PC connected callback for network probing\n");

    // 3. 以拉流端身份加入房间
    const char* room_id = ctx->room_id ? ctx->room_id : "default_room";
    if ((ret = p2p_join_room_as_receiver(p2p_ctx, room_id)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to join room as receiver\n");
        return ret;
    }

    return 0;
}

static int p2p_rtp_read_header(AVFormatContext *avctx) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;
    P2PContext *p2p_ctx = &ctx->p2p_context;
    int ret;

    av_log(avctx, AV_LOG_INFO, "Starting P2P RTP demux initialization\n");
    p2p_ctx->avctx = avctx;

    // 初始化P2P连接
    if ((ret = p2p_rtp_demux_init(avctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to p2p_rtp_demux_init\n");
        goto fail;
    }

    // 等待推流状态就绪
    if ((ret = pending_for_probing_node(ctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to reach streaming state\n");
        goto fail;
    }

    av_log(avctx, AV_LOG_INFO, "P2P RTP demux ready to receive stream from %s\n", p2p_ctx->selected_node->remote_id);
    return 0;

fail:
    return ret;
}

static int p2p_rtp_read_packet(AVFormatContext *avctx, AVPacket *pkt) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;
    PeerConnectionNode *node = ctx->p2p_context.selected_node;
    
    if (!node) {
        av_log(avctx, AV_LOG_ERROR, "No selected node for reading packet\n");
        return AVERROR(EINVAL);
    }
    
    if (ctx->current_state != RECEIVER_STATE_STREAMING) {
        av_log(avctx, AV_LOG_ERROR, "Not in streaming state\n");
        return AVERROR(EINVAL);
    }
    
    PeerConnectionTrack* track = find_peer_connection_track_by_stream_index(node->track_caches, 0);
    if (!track || !track->rtp_ctx) {
        av_log(avctx, AV_LOG_ERROR, "No valid track found for stream index 0\n");
        return AVERROR(EINVAL);
    }

    pkt->stream_index = 0;
    int ret = av_read_frame(track->rtp_ctx, pkt);
    
    return ret;
}

static int p2p_rtp_close(AVFormatContext *avctx) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;
    
    av_log(avctx, AV_LOG_INFO, "Closing P2P RTP demux\n");
    
    p2p_close_resource(&ctx->p2p_context);
    
    if (ctx->room_id) {
        av_freep(&ctx->room_id);
    }
    
    if (ctx->p2p_context.signal_callbacks) {
        av_freep(&ctx->p2p_context.signal_callbacks);
    }
    
    return 0;
}

// ==================== 状态管理函数 ====================

static void set_receiver_state(P2PRtpDemuxContext* ctx, ReceiverState new_state) {
    // xy:Todo: 接收端暂时用不到加锁
    if (ctx->current_state < new_state) {
        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
               "Receiver state change: %s -> %s\n", get_receiver_state_name(ctx->current_state), get_receiver_state_name(new_state));
        ctx->current_state = new_state;
        ctx->state_enter_time = av_gettime_relative();
    }
}

static int check_state_timeout(P2PRtpDemuxContext* ctx, int64_t timeout_us) {
    return (av_gettime_relative() - ctx->state_enter_time) > timeout_us;
}

// ==================== 信令消息处理回调 ====================

static int p2p_rtp_demux_signal_handler(P2PContext* ctx, const P2PSignalMessage* msg) {
    int ret;
    P2PRtpDemuxContext* demux_ctx = (P2PRtpDemuxContext*)ctx->avctx->priv_data;
    char* remote_id = msg->sender_id;
    
    av_log(ctx->avctx, AV_LOG_DEBUG, "Demux processing signal message type: %d from: %s\n", 
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
                
                set_receiver_state(demux_ctx, RECEIVER_STATE_WAITING_FOR_SENDER);
            }
            break;
            
        case P2P_MSG_CONNECT_REQUEST:
            {
                const char* target_id = msg->payload.connect_request.target_id;
                av_log(ctx->avctx, AV_LOG_INFO, "Received connect request to %s\n", target_id);

                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, target_id);
                if (node == NULL) {
                    // int ret = init_peer_connection(ctx, target_id);
                    ret = create_peer_connection(ctx, remote_id);//init_peer_connection(ctx, remote_id);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create peer connection for %s\n", target_id);
                        return ret;
                    }

                    node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, target_id);
                }

            /*
                // 参考mux端：一次性创建probe、video、audio track
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
                
                // 2. 预创建视频track占位符（具体参数待SDP协商确定，不调用init_recv_track_ex）
                if (!node->video_track) {
                    PeerConnectionTrack* video_track = av_mallocz(sizeof(PeerConnectionTrack));
                    if (!video_track) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate video track\n");
                        return AVERROR(ENOMEM);
                    }
                    video_track->track_type = PeerConnectionTrackType_Video;
                    video_track->stream_index = -1;
                    node->video_track = video_track;
                    av_log(ctx->avctx, AV_LOG_INFO, "Created video track placeholder for receiver %s\n", target_id);
                }
                
                // 3. 预创建音频track占位符（具体参数待SDP协商确定，不调用init_recv_track_ex）
                if (!node->audio_track) {
                    PeerConnectionTrack* audio_track = av_mallocz(sizeof(PeerConnectionTrack));
                    if (!audio_track) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate audio track\n");
                        return AVERROR(ENOMEM);
                    }
                    audio_track->track_type = PeerConnectionTrackType_Audio;
                    audio_track->stream_index = -1;
                    node->audio_track = audio_track;
                    av_log(ctx->avctx, AV_LOG_INFO, "Created audio track placeholder for receiver %s\n", target_id);
                }
                
                av_log(ctx->avctx, AV_LOG_INFO, "All track placeholders created for %s (will be fully initialized after SDP negotiation)\n", target_id);

                // 获取PeerConnection的本地SDP描述（而不是特定track，因为probe track是DataChannel）
                int sdp_size = 4096; // 预设一个足够大的缓冲区大小
                char* sdp = av_malloc(sdp_size);
                if (sdp) {
                    // 对于DataChannel（probe track），我们获取整个peer connection的本地描述
                    int ret = rtcGetLocalDescription(node->pc_id, sdp, sdp_size);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "rtcGetLocalDescription failed for pc_id: %d\n", node->pc_id);
                        av_free(sdp);
                        sdp = NULL;
                    } else {
                        av_log(ctx->avctx, AV_LOG_DEBUG, "Got local SDP for peer connection %d\n", node->pc_id);
                    }
                } else {
                    av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate memory for SDP\n");
                    return AVERROR(ENOMEM);
                }
                //xy:Todo: 这里需要改造！只有回调里面才能收到目标sdp

                // 发送信令服务器
                P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_OFFER, ctx->local_id, target_id); 
                msg->payload.offer.description = sdp;
                p2p_send_signal_message(ctx, msg);
                p2p_free_signal_message(msg);
            */
                init_peer_connection(ctx, node->pc_id);
                set_receiver_state(demux_ctx, RECEIVER_STATE_SIGNALING);
            }
            break;
            
        case P2P_MSG_OFFER:
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node == NULL) {
                    // int ret = init_peer_connection(ctx, remote_id);
                    ret = create_peer_connection(ctx, remote_id);//init_peer_connection(ctx, remote_id);
                    if (ret == 0) {
                        node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                        av_log(ctx->avctx, AV_LOG_INFO, "Created new peer connection for sender %s\n", remote_id);
                    } else {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create peer connection for %s\n", remote_id);
                        return ret;
                    }
                }
                
                if (node && msg->payload.offer.description) {
                    av_log(ctx->avctx, AV_LOG_DEBUG, "Processing Offer SDP from %s, sdp: %s\n", remote_id, msg->payload.offer.description);
                    rtcSetRemoteDescription(node->pc_id, msg->payload.offer.description, "offer");
                    av_log(ctx->avctx, AV_LOG_INFO, "Offer SDP processed, tracks initialized for %s\n", remote_id);
                    init_peer_connection(ctx, node->pc_id);
                    send_local_description(ctx, node, "answer");
                    set_receiver_state(demux_ctx, RECEIVER_STATE_SIGNALING);
                }
            }
            break;
            
        case P2P_MSG_ANSWER:
            // 处理Answer
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
            // 处理ICE候选
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
        /*
        case P2P_MSG_PROBE_RESPONSE:
            // 处理探测响应
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Received probe response from %s, probe_id=%s, status=%s\n", 
                           remote_id, msg->payload.probe_response.probe_id, msg->payload.probe_response.status);
                    
                    if (strcmp(msg->payload.probe_response.status, "ok") == 0) {
                        // set_receiver_state(demux_ctx, RECEIVER_STATE_PROBE_COMPLETED);
                    }
                }
            }
            break;
        */
        case P2P_MSG_STREAM_READY:
            // 推流端准备就绪
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                
                if (node && node == ctx->selected_node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Stream ready from %s, stream_id=%s\n", 
                           remote_id, msg->payload.stream_ready.stream_id ? msg->payload.stream_ready.stream_id : "unknown");
                    
                    set_receiver_state(demux_ctx, RECEIVER_STATE_STREAMING);
                } else {
                    av_log(ctx->avctx, AV_LOG_WARNING, "Received stream ready from non-selected node %s\n", remote_id);
                }
            }
            break;
            
        case P2P_MSG_ERROR:
            // 处理错误消息
            {
                av_log(ctx->avctx, AV_LOG_ERROR, "Received error from %s: code=%s, msg=%s\n",
                       remote_id ? remote_id : "unknown",
                       msg->payload.error.error_code ? msg->payload.error.error_code : "unknown",
                       msg->payload.error.error_msg ? msg->payload.error.error_msg : "unknown");
                set_receiver_state(demux_ctx, RECEIVER_STATE_ERROR);
            }
            break;
            
        default:
            av_log(ctx->avctx, AV_LOG_DEBUG, "Unhandled message type: %d\n", msg->type);
            break;
    }
    
    return 0;
}

static int handle_datachannel_ready(P2PRtpDemuxContext* ctx, PeerConnectionNode* node) {
    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, "DataChannel ready for node %s\n", node->remote_id);
    
    probe_data_init(&node->network_quality);
    
    if (!node->probe_track || node->probe_track->track_id <= 0) {
        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "Probe track not available for node %s\n", node->remote_id);
        return AVERROR(EINVAL);
    }
    
    set_receiver_state(ctx, RECEIVER_STATE_DATACHANNEL_READY);
    return 0;
}

static int pending_for_probing_node(P2PRtpDemuxContext* ctx) {
    int64_t timeout_us = 10000* 1000000LL;;//(ctx->wait_timeout ? ctx->wait_timeout : 30) * 1000000LL; // 默认30秒超时
    int64_t start_time = av_gettime_relative();
    
    while (ctx->current_state != RECEIVER_STATE_STREAMING) {
        // 检查总体超时
        if (av_gettime_relative() - start_time > timeout_us) {
            av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "Timeout waiting for streaming state after %d seconds\n", 
                   ctx->wait_timeout ? ctx->wait_timeout : 30);
            return AVERROR(ETIMEDOUT);
        }
        
        if (ctx->current_state == RECEIVER_STATE_ERROR) {
            av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "Receiver entered error state\n");
            return AVERROR_EXTERNAL;
        }
        
        // 状态机处理
        switch (ctx->current_state) {
            case RECEIVER_STATE_DATACHANNEL_READY:
                // DataChannel就绪后，初始化所有节点的探测结构并发送探测请求
                {
                    int connected_nodes = 0;
                    int probe_sent = 0;
                    
                    // 第一步：初始化所有已连接节点的探测结构
                    PeerConnectionNode* curr = ctx->p2p_context.peer_connection_node_caches;
                    while (curr) {
                        if (curr->status == Connected) {
                            connected_nodes++;
                            // 初始化探测数据结构
                            probe_data_init(&curr->network_quality);
                            // probe_track已在之前预创建，无需重复创建探测通道
                            if (curr->probe_track && curr->probe_track->track_id > 0) {
                                av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                                       "Probe track available for node %s\n", curr->remote_id);
                            } else {
                                av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, 
                                       "Probe track not available for node %s\n", curr->remote_id);
                            }
                        }
                        curr = curr->next;
                    }
                    
                    if (connected_nodes == 0) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, "No connected nodes found, waiting...\n");
                        av_usleep(100000); // 100ms
                        break;
                    }
                    
                    // 第二步：向所有已连接节点发送探测请求
                    curr = ctx->p2p_context.peer_connection_node_caches;
                    while (curr) {
                        if (curr->status == Connected) {
                            int ret = send_probe_request_to_publisher(&ctx->p2p_context, curr);
                            if (ret >= 0) {
                                probe_sent++;
                                av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                                       "Sent probe request to node %s\n", curr->remote_id);
                            } else {
                                av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, 
                                       "Failed to send probe request to node %s\n", curr->remote_id);
                            }
                        }
                        curr = curr->next;
                    }
                    
                    while (probe_sent == 0) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, 
                               "No probe requests sent, retrying...\n");
                        av_usleep(500000); // 500ms等待后重试
                    }
                    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                            "Sent probe requests to %d/%d nodes, entering probing state\n", 
                            probe_sent, connected_nodes);
                    set_receiver_state(ctx, RECEIVER_STATE_PROBING);
                }
                break;
                
            case RECEIVER_STATE_PROBING:
                // 探测中，使用while循环持续检查所有已连接节点的探测状态
                {
                    int64_t probe_timeout = 30 * 1000000LL; // 30秒探测超时
                    int64_t probe_start_time = ctx->state_enter_time;
                    
                    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                           "Entering probe pending mode, will check every 1 second\n");
                    
                    // 使用while循环持续检查探测状态
                    while (ctx->current_state == RECEIVER_STATE_PROBING) {
                        int total_nodes = 0;
                        int completed_nodes = 0;
                        int64_t current_time = av_gettime_relative();
                        int64_t elapsed_since_probe_start = current_time - probe_start_time;
                        
                        // 检查是否总体超时
                        if (elapsed_since_probe_start > probe_timeout) {
                            av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, 
                                   "Probe phase timeout after %lld seconds, forcing completion\n", 
                                   elapsed_since_probe_start / 1000000);
                            
                            // 强制完成所有未完成的节点
                            PeerConnectionNode* curr = ctx->p2p_context.peer_connection_node_caches;
                            while (curr) {
                                if (curr->status == Connected && curr->network_quality.phase != PROBING_COMPLETE) {
                                    curr->network_quality.phase = PROBING_COMPLETE;
                                    av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, 
                                           "Force completing probe for node %s due to timeout\n", curr->remote_id);
                                }
                                curr = curr->next;
                            }
                            break; // 退出while循环，将在下面转换状态
                        }
                        
                        // 轮询检查所有已连接节点的探测状态
                        PeerConnectionNode* curr = ctx->p2p_context.peer_connection_node_caches;
                        while (curr) {
                            if (curr->status == Connected) {
                                total_nodes++;
                                
                                // 检查节点探测状态
                                if (curr->network_quality.phase == PROBING_COMPLETE) {
                                    completed_nodes++;
                                    av_log(ctx->p2p_context.avctx, AV_LOG_DEBUG, 
                                           "Node %s: probe completed\n", curr->remote_id);
                                } else {
                                    // 检查单个节点探测超时
                                    if (curr->network_quality.timeout_ms > 0 && 
                                        curr->network_quality.probe_start_time > 0) {
                                        int64_t node_elapsed_ms = (current_time - curr->network_quality.probe_start_time) / 1000;
                                        if (node_elapsed_ms >= curr->network_quality.timeout_ms) {
                                            // 单节点探测超时，标记为完成
                                            curr->network_quality.phase = PROBING_COMPLETE;
                                            completed_nodes++;
                                            av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, 
                                                   "Node %s: probe timeout after %lldms (expected %dms)\n",
                                                   curr->remote_id, node_elapsed_ms, curr->network_quality.timeout_ms);
                                        } else {
                                            av_log(ctx->p2p_context.avctx, AV_LOG_DEBUG, 
                                                   "Node %s: probing in progress (%lld/%d ms)\n",
                                                   curr->remote_id, node_elapsed_ms, curr->network_quality.timeout_ms);
                                        }
                                    } else {
                                        av_log(ctx->p2p_context.avctx, AV_LOG_DEBUG, 
                                               "Node %s: probe not started or no timeout set\n", curr->remote_id);
                                    }
                                }
                            }
                            curr = curr->next;
                        }
                        
                        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                               "Probe polling result: %d/%d nodes completed (elapsed: %lld ms)\n", 
                               completed_nodes, total_nodes, elapsed_since_probe_start / 1000);
                        
                        // 检查是否满足探测完成条件
                        if (total_nodes == 0) {
                            av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, 
                                   "No connected nodes found during probing\n");
                            set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                            return AVERROR(EINVAL);
                        } else if (completed_nodes >= total_nodes) {
                            // 所有节点探测完成
                            av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                                   "All %d nodes completed probing, exiting probe pending mode\n", total_nodes);
                            break; // 退出while循环，将在下面转换状态
                        } else {
                            // 继续等待，1秒后再次检查
                            av_log(ctx->p2p_context.avctx, AV_LOG_DEBUG, 
                                   "Probe pending: %d/%d completed, sleeping 1s for next check\n", 
                                   completed_nodes, total_nodes);
                            av_usleep(100000); // 100ms后再次检查
                        }
                    }
                    
                    // 退出while循环后，转换到下一状态
                    if (ctx->current_state == RECEIVER_STATE_PROBING) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                               "Probe pending completed, moving to selection phase\n");
                        set_receiver_state(ctx, RECEIVER_STATE_PROBE_COMPLETED);
                    }
                }
                break;
                
            case RECEIVER_STATE_PROBE_COMPLETED:
                // 探测完成，进行最优节点选择
                {
                    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, "Starting best node selection process\n");
                    
                    // 统计可用节点
                    int available_nodes = 0;
                    PeerConnectionNode* curr = ctx->p2p_context.peer_connection_node_caches;
                    while (curr) {
                        if (curr->status == Connected && curr->network_quality.phase == PROBING_COMPLETE) {
                            available_nodes++;
                            av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                                   "Available node %s: score=%.2f\n", 
                                   curr->remote_id, curr->network_quality.final_score);
                        }
                        curr = curr->next;
                    }
                    
                    if (available_nodes == 0) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, 
                               "No available nodes for selection\n");
                        set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return AVERROR(EINVAL);
                    }
                    
                    // 选择最优节点
                    PeerConnectionNode* best_node = select_best_node(&ctx->p2p_context);
                    if (!best_node) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, 
                               "Failed to select best node from %d available nodes\n", available_nodes);
                        set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return AVERROR(EINVAL);
                    }
                    
                    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                           "Selected best node: %s (score=%.2f) from %d candidates\n", 
                           best_node->remote_id, best_node->network_quality.final_score, available_nodes);
                    
                    // 设置选中的节点
                    ctx->p2p_context.selected_node = best_node;
                    best_node->status = Selected;
                    
                    // 立即进入请求推流状态
                    set_receiver_state(ctx, RECEIVER_STATE_REQUESTING_STREAM);
                }
                break;
                
            case RECEIVER_STATE_REQUESTING_STREAM:
                // 向选中的节点发送推流请求
                {
                    if (!ctx->p2p_context.selected_node) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "No selected node for stream request\n");
                        set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return AVERROR(EINVAL);
                    }
                    
                    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                           "Sending stream request to selected node %s\n", 
                           ctx->p2p_context.selected_node->remote_id);
                    
                    int ret = send_stream_request(&ctx->p2p_context, ctx->p2p_context.selected_node);
                    if (ret >= 0) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                               "Stream request sent successfully to %s, waiting for stream ready\n", 
                               ctx->p2p_context.selected_node->remote_id);
                        // 保持在REQUESTING_STREAM状态，等待P2P_MSG_STREAM_READY消息
                    } else {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, 
                               "Failed to send stream request to %s: %s\n",
                               ctx->p2p_context.selected_node->remote_id, av_err2str(ret));
                        set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return ret;
                    }
                    
                    av_usleep(100000); // 100ms
                }
                break;
                
            default:
                // 其他状态，短暂等待
                av_usleep(100000); // 100ms
                break;
        }
    }
    
    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, "Successfully reached streaming state\n");
    return 0;
}

static void on_datachannel_established(P2PRtpDemuxContext* ctx, PeerConnectionNode* node) {
    if (ctx->current_state == RECEIVER_STATE_P2P_CONNECTING) {
        handle_datachannel_ready(ctx, node);
    }
}

// ==================== P2P连接成功后的回调处理 ====================

static void on_pc_connected_callback(P2PContext* ctx, PeerConnectionNode* node) {
    P2PRtpDemuxContext* demux_ctx = (P2PRtpDemuxContext*)ctx->avctx->priv_data;
    
    av_log(ctx->avctx, AV_LOG_INFO, "P2P connection established with sender %s, starting network probing\n", node->remote_id);
    
    probe_data_init(&node->network_quality);
    
    set_receiver_state(demux_ctx, RECEIVER_STATE_DATACHANNEL_READY);//xy:Todo: 待调试
    
    av_log(ctx->avctx, AV_LOG_INFO, "Receiver state set to DATACHANNEL_READY, will send probe request to %s\n", node->remote_id);
}

// ==================== 辅助函数 ====================

static int p2p_join_room_as_receiver(P2PContext* p2p_ctx, const char* room_id) {
    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_JOIN_ROOM, p2p_ctx->local_id, NULL);
    if (!msg) {
        av_log(p2p_ctx->avctx, AV_LOG_ERROR, "Failed to allocate P2PSignalMessage\n");
        return AVERROR(ENOMEM);
    }
    
    msg->payload.join_room.role = strdup("receiver");
    if (room_id)
        msg->payload.join_room.room_id = strdup(room_id);
    else
        msg->payload.join_room.room_id = strdup("default_room");
    
    // 添加拉流端能力描述
    msg->payload.join_room.capabilities = strdup("video:h264,h265;audio:aac,opus;max_bitrate:10000kbps");
    
    int ret = p2p_send_signal_message(p2p_ctx, msg);
    
    av_log(p2p_ctx->avctx, AV_LOG_INFO, "Joining room as receiver: %s\n", room_id);
    
    p2p_free_signal_message(msg);
    return ret;
}

// ==================== 配置选项 ====================

#define OFFSET(x) offsetof(P2PRtpDemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    { "room_id", "Room ID to join", OFFSET(room_id), AV_OPT_TYPE_STRING, { .str = "default_room" }, 0, 0, DEC },
    { "wait_timeout", "Timeout for waiting streaming (seconds)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, { .i64 = P2P_WAITING_RECEIVER_TIMEOUT_SEC }, 5, 300, DEC },
    { NULL },
};

static const AVClass p2p_rtp_demuxer_class = {
    .class_name = "P2P RTP demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_p2p_rtp_demuxer = {
    .name           = "p2pdemuxer",
    .long_name      = NULL_IF_CONFIG_SMALL("P2P RTP 解复用器"),
    .flags          = AVFMT_NOFILE | AVFMT_EXPERIMENTAL,
    .priv_class     = &p2p_rtp_demuxer_class,
    .priv_data_size = sizeof(P2PRtpDemuxContext),
    .read_probe     = NULL,
    .read_header    = p2p_rtp_read_header,
    .read_packet    = p2p_rtp_read_packet,
    .read_close     = p2p_rtp_close,
};
