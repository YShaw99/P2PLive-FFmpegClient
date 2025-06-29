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

// ==================== 前向声明 ====================
static void set_receiver_state(P2PRtpDemuxContext* ctx, ReceiverState new_state);
static int p2p_rtp_demux_signal_handler(P2PContext* ctx, const P2PSignalMessage* msg);
static int p2p_join_room_as_receiver(P2PContext* p2p_ctx, const char* room_id);
static int handle_datachannel_ready(P2PRtpDemuxContext* ctx, PeerConnectionNode* node);
static void on_datachannel_established(P2PRtpDemuxContext* ctx, PeerConnectionNode* node);
static int check_state_timeout(P2PRtpDemuxContext* ctx, int64_t timeout_us);
static int pending_for_probing_node(P2PRtpDemuxContext* ctx);

// ==================== 主要初始化函数 ====================
static int p2p_rtp_demux_init(AVFormatContext *avctx) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;
    int ret;

    // 初始化状态
    p2p_ctx->avctx = avctx;
    set_receiver_state(ctx, RECEIVER_STATE_IDLE);
    
    // 生成本地ID
    //xy:todo:删掉
    char local_id[64];
    snprintf(local_id, sizeof(local_id), "recv");//%08x", av_get_random_seed());
    p2p_ctx->local_id = strdup(local_id);
    
    av_log(avctx, AV_LOG_INFO, "P2P Receiver initialized with ID: %s\n", p2p_ctx->local_id);

    set_receiver_state(ctx, RECEIVER_STATE_JOINING_ROOM);

    // 1. 连接信令服务器
    if ((ret = p2p_init_signal_server(p2p_ctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init signal server\n");
        return ret;
    }

    // 2. 设置信令消息处理回调
    ret = p2p_set_signal_message_handler(p2p_ctx, p2p_rtp_demux_signal_handler, ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set signal message handler\n");
        return ret;
    }

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
    char media_stream_id[37];
    PeerConnectionNode *pc_node = NULL;
    int ret;

    av_log(avctx, AV_LOG_INFO, "Starting P2P RTP demux initialization\n");

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

    pc_node = p2p_ctx->selected_node;
    if (!pc_node) {
        av_log(avctx, AV_LOG_ERROR, "No selected node available\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }
    
    pc_node->p2p_ctx = p2p_ctx;

    // 生成媒体流ID
    if ((ret = p2p_generate_media_stream_id(media_stream_id)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        goto fail;
    }

    // 媒体tracks现在在SDP协商阶段预创建，在track回调中完成初始化

    av_log(avctx, AV_LOG_INFO, "P2P RTP demux ready to receive stream from %s\n", pc_node->remote_id);
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

    pkt->stream_index = 0;  // 确保stream_index正确
    int ret = av_read_frame(track->rtp_ctx, pkt);
    
    return ret;
}

static int p2p_rtp_close(AVFormatContext *avctx) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;
    
    av_log(avctx, AV_LOG_INFO, "Closing P2P RTP demux\n");
    
    // 清理资源
    p2p_close_resource(&ctx->p2p_context);
    
    if (ctx->room_id) {
        av_freep(&ctx->room_id);
    }
    
    return 0;
}

// ==================== 状态管理函数 ====================

static void set_receiver_state(P2PRtpDemuxContext* ctx, ReceiverState new_state) {
    if (ctx->current_state != new_state) {
        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
               "Receiver state change: %d -> %d\n", ctx->current_state, new_state);
        ctx->current_state = new_state;
        ctx->state_enter_time = av_gettime_relative();
    }
}

static int check_state_timeout(P2PRtpDemuxContext* ctx, int64_t timeout_us) {
    return (av_gettime_relative() - ctx->state_enter_time) > timeout_us;
}

// ==================== 信令消息处理回调 ====================

static int p2p_rtp_demux_signal_handler(P2PContext* ctx, const P2PSignalMessage* msg) {
    P2PRtpDemuxContext* demux_ctx = (P2PRtpDemuxContext*)ctx->avctx->priv_data;
    
    av_log(ctx->avctx, AV_LOG_DEBUG, "Demux processing signal message type: %d from: %s\n", 
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
                
                set_receiver_state(demux_ctx, RECEIVER_STATE_WAITING_FOR_SENDER);
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
                    //xy:TODO 待调试

                // 如果node还没有创建tracks，先创建它们（接收端预创建） 接收端可以预先创建probe track，媒体tracks等待SDP协商后创建
                if (!node->probe_track) {
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
                    av_log(ctx->avctx, AV_LOG_INFO, "Created and initialized probe track for receiver %s\n", target_id);
                    
                    // 注意：接收端的媒体tracks需要等待SDP协商完成后才能创建
                    // 因为需要从SDP中获取codec、payload type、SSRC等参数
                    av_log(ctx->avctx, AV_LOG_INFO, "Media tracks will be created after SDP negotiation for %s\n", target_id);
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

                set_receiver_state(demux_ctx, RECEIVER_STATE_SIGNALING);
            }
            break;
            
        case P2P_MSG_OFFER:
            // 收到推流端的Offer
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                if (node == NULL) {
                    // 新建 PeerConnection
                    int ret = init_peer_connection(ctx, msg->id);
                    if (ret == 0) {
                        node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                        av_log(ctx->avctx, AV_LOG_INFO, "Created new peer connection for sender %s\n", msg->id);
                    } else {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create peer connection for %s\n", msg->id);
                        return ret;
                    }
                }
                
                if (node && msg->payload.offer.description) {
                    rtcSetRemoteDescription(node->pc_id, msg->payload.offer.description, "offer");
                    
                    //xy:TODO 待调试
                    // // 收到Offer SDP后，预创建接收用的媒体tracks
                    // // 注意：这里我们创建标准的视频和音频tracks，具体参数将在track回调中确定
                    // if (!node->video_track && !node->audio_track) {
                    //     av_log(ctx->avctx, AV_LOG_INFO, "Creating receiving tracks for sender %s after SDP negotiation\n", msg->id);
                        
                    //     // 创建视频接收track占位符（具体参数待track回调确定）
                    //     PeerConnectionTrack* video_track = av_mallocz(sizeof(PeerConnectionTrack));
                    //     if (video_track) {
                    //         video_track->track_type = PeerConnectionTrackType_Video;
                    //         video_track->stream_index = 0;
                    //         node->video_track = video_track;
                    //         av_log(ctx->avctx, AV_LOG_INFO, "Created video track placeholder for %s\n", msg->id);
                    //     }
                        
                    //     // 创建音频接收track占位符（具体参数待track回调确定）
                    //     PeerConnectionTrack* audio_track = av_mallocz(sizeof(PeerConnectionTrack));
                    //     if (audio_track) {
                    //         audio_track->track_type = PeerConnectionTrackType_Audio;
                    //         audio_track->stream_index = 1;
                    //         node->audio_track = audio_track;
                    //         av_log(ctx->avctx, AV_LOG_INFO, "Created audio track placeholder for %s\n", msg->id);
                    //     }
                        
                    //     av_log(ctx->avctx, AV_LOG_INFO, "Media track placeholders created for %s (will be fully initialized in track callback)\n", msg->id);
                    // }
                    
                    set_receiver_state(demux_ctx, RECEIVER_STATE_SIGNALING);
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
            
        case P2P_MSG_PROBE_RESPONSE:
            // 处理探测响应
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                if (node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Received probe response from %s, probe_id=%s, status=%s\n", 
                           msg->id, msg->payload.probe_response.probe_id, msg->payload.probe_response.status);
                    
                    if (strcmp(msg->payload.probe_response.status, "ok") == 0) {
                        //Todo:xy: 这里应该计算该node的网络情况，例如停止计时，计算rtt
                        // 探测成功，标记探测阶段完成
                        node->network_quality.phase = PROBING_COMPLETE;
                        
                        //Todo:xy: 这里应该有一个通知，所有节点都收到通知或者超时后，再转变为RECEIVER_STATE_PROBE_COMPLETED状态，计算最优节点。
                        // av_log(ctx->avctx, AV_LOG_INFO, "Network probing completed, selecting best node\n");
                        
                        // PeerConnectionNode* best_node = select_best_node(ctx);
                        // if (!best_node) {
                        //     av_log(ctx->avctx, AV_LOG_ERROR, "Failed to select best node\n");
                        //     set_receiver_state(demux_ctx, RECEIVER_STATE_ERROR);
                        //     return AVERROR(EINVAL);
                        // }
                        
                        // av_log(ctx->avctx, AV_LOG_INFO, "Selected best node %s with score %.2f\n", 
                        //        best_node->remote_id, best_node->network_quality.final_score);
                        
                        // 设置选中的节点
                        // ctx->selected_node = best_node;
                        // best_node->status = Selected;
                        // set_receiver_state(demux_ctx, RECEIVER_STATE_PROBE_COMPLETED);
                    }
                }
            }
            break;
            
        case P2P_MSG_STREAM_READY:
            // 推流端准备就绪
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, msg->id);
                
                if (node && node == ctx->selected_node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Stream ready from %s, stream_id=%s\n", 
                           msg->id, msg->payload.stream_ready.stream_id ? msg->payload.stream_ready.stream_id : "unknown");
                    
                    set_receiver_state(demux_ctx, RECEIVER_STATE_STREAMING);
                    ctx->waiting_for_sender = 0;  // Todo:xy: 这个标记位后续可以删掉了，改用state管理
                } else {
                    av_log(ctx->avctx, AV_LOG_WARNING, "Received stream ready from non-selected node %s\n", msg->id);
                }
            }
            break;
            
        case P2P_MSG_ERROR:
            // 处理错误消息
            {
                av_log(ctx->avctx, AV_LOG_ERROR, "Received error from %s: code=%s, msg=%s\n",
                       msg->id ? msg->id : "unknown",
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
    
    // 初始化探测数据结构
    probe_data_init(&node->network_quality);
    
    // 创建探测通道
    int ret = create_probe_channel(node);
    if (ret < 0) {
        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "Failed to create probe channel\n");
        return ret;
    }
    
    set_receiver_state(ctx, RECEIVER_STATE_DATACHANNEL_READY);
    return 0;
}

static int pending_for_probing_node(P2PRtpDemuxContext* ctx) {
    int64_t timeout_us = (ctx->wait_timeout ? ctx->wait_timeout : 30) * 1000000LL; // 默认30秒超时
    int64_t start_time = av_gettime_relative();
    
    while (ctx->current_state != RECEIVER_STATE_STREAMING) {
        if (av_gettime_relative() - start_time > timeout_us) {
            av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "Timeout waiting for streaming state\n");
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
                            // 创建探测通道
                            if (create_probe_channel(curr) >= 0) {
                                //Todo:xy: 创建了channel之后该如何update？
                                av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                                       "Initialized probe structure for node %s\n", curr->remote_id);
                            }
                        }
                        curr = curr->next;
                    }
                    
                    if (connected_nodes == 0) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, "No connected nodes found\n");
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
                    
                    if (probe_sent > 0) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                               "Sent probe requests to %d/%d nodes, entering probing state\n", 
                               probe_sent, connected_nodes);
                        set_receiver_state(ctx, RECEIVER_STATE_PROBING);
                    }
                }
                break;
                
            case RECEIVER_STATE_PROBING:
                // 探测中，检查是否所有节点都完成了探测
                {
                    int total_nodes = 0;
                    int completed_nodes = 0;
                    
                    PeerConnectionNode* curr = ctx->p2p_context.peer_connection_node_caches;
                    while (curr) {
                        if (curr->status == Connected) {
                            total_nodes++;
                            if (curr->network_quality.phase == PROBING_COMPLETE) {
                                completed_nodes++;
                            }
                        }
                        curr = curr->next;
                    }
                    
                    av_log(ctx->p2p_context.avctx, AV_LOG_DEBUG, 
                           "Probing progress: %d/%d nodes completed\n", completed_nodes, total_nodes);
                    
                    // 如果所有节点都完成探测，或者探测超时（比如10秒）
                    int64_t probe_timeout = 10 * 1000000LL; // 10秒探测超时
                    if (completed_nodes == total_nodes || 
                        (av_gettime_relative() - ctx->state_enter_time) > probe_timeout) {
                        
                        if (completed_nodes < total_nodes) {
                            av_log(ctx->p2p_context.avctx, AV_LOG_WARNING, 
                                   "Probe timeout, only %d/%d nodes completed\n", completed_nodes, total_nodes);
                        }
                        
                        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                               "Network probing completed, selecting best node\n");
                        
                        // 选择最优节点
                        PeerConnectionNode* best_node = select_best_node(&ctx->p2p_context);
                        if (!best_node) {
                            av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "Failed to select best node\n");
                            set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                            return AVERROR(EINVAL);
                        }
                        
                        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                               "Selected best node %s with score %.2f\n", 
                               best_node->remote_id, best_node->network_quality.final_score);
                        
                        // 设置选中的节点
                        ctx->p2p_context.selected_node = best_node;
                        best_node->status = Selected;
                        set_receiver_state(ctx, RECEIVER_STATE_PROBE_COMPLETED);
                    }
                }
                break;
                
            case RECEIVER_STATE_PROBE_COMPLETED:
                // 探测完成后，发送推流请求
                {
                    if (!ctx->p2p_context.selected_node) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "No selected node for stream request\n");
                        set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return AVERROR(EINVAL);
                    }
                    
                    int ret = send_stream_request(&ctx->p2p_context, ctx->p2p_context.selected_node);
                    if (ret >= 0) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_INFO, 
                               "Sent stream request to selected node %s\n", 
                               ctx->p2p_context.selected_node->remote_id);
                        set_receiver_state(ctx, RECEIVER_STATE_REQUESTING_STREAM);
                    } else {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, 
                               "Failed to send stream request\n");
                        set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return ret;
                    }
                }
                break;
                
            default:
                break;
        }
        
        av_usleep(100000); // 100ms
    }
    
    return 0;
}

static void on_datachannel_established(P2PRtpDemuxContext* ctx, PeerConnectionNode* node) {
    if (ctx->current_state == RECEIVER_STATE_P2P_CONNECTING) {
        handle_datachannel_ready(ctx, node);
    }
}

// ==================== 辅助函数 ====================

static int p2p_join_room_as_receiver(P2PContext* p2p_ctx, const char* room_id) {
    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_JOIN_ROOM, NULL);
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
    { "wait_timeout", "Timeout for waiting streaming (seconds)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, { .i64 = 30 }, 5, 300, DEC },
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
