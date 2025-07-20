
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
#include "libavutil/dict.h"
#include "libavcodec/packet.h"
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
#include "libavcodec/aac.h"
#include <pthread.h>
#include <sys/time.h>
#include <string.h>
#include "libavutil/intreadwrite.h"

#include "../libavutil/error.h"
#include "../libavutil/log.h"


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

typedef enum SwitchTarget {
    SWITCH_NONE = 0,
    SWITCH_TO_P2P,
    SWITCH_TO_CDN,
} SwitchTarget;

typedef enum ActiveSource {
    ACTIVE_NONE = 0,
    ACTIVE_CDN,
    ACTIVE_P2P,
} ActiveSource;

typedef struct P2PRtpDemuxSwitchState {
    ActiveSource active_source;     // 当前输出源
    SwitchTarget pending_switch;    // 待切换目标

    AVPacket pending_pkt;           // 用于在检测到关键帧时缓存一帧，保证切换从关键帧开始
    int pending_pkt_valid;          // 是否已有缓存包
    ActiveSource pending_pkt_from;  // 缓存包来源
    int pending_pkt_out_stream_index; // 缓存包目标输出流下标（0:video,1:audio）
    AVRational pending_pkt_src_tb;  // 缓存包原 time_base
} P2PRtpDemuxSwitchState;

typedef struct P2PRtpDemuxContext {
    AVClass *av_class;
    P2PContext p2p_context;

    int output_video_stream_index;  //Todo:上层暂不支持主动切流播放，这里
    int output_audio_stream_index;
    // CDN相关
    const char *cdn_url; // CDN 源
    AVFormatContext *cdn_fmt_ctx; // WHEP 内部 demux 上下文
    int cdn_video_stream_index;      // 主输出视频流下标（本 demuxer 内）
    int cdn_audio_stream_index;      // 主输出音频流下标（本 demuxer 内）

    //xy:todo:回退目前存在bug，待修复。
    int64_t stall_switch_ms;    // 卡顿阈值(ms)，超过则切换
    int cdn_active;        // 当前是否已经切到 WHEP
    int64_t last_p2p_packet_time; // 最近一次成功拿到 P2P 包的时间

    // P2P相关
    ReceiverState current_state;
    int64_t state_enter_time;
    char* room;                 // 房间ID（等同于 CDN URL 中的 stream）
    int wait_timeout;           // 等待超时时间（秒）
    int p2p_transport_error_times;

    // 并行建立 P2P 相关
    pthread_t p2p_connect_thread;
    int p2p_connect_thread_active;
    int p2p_connect_ready;      // Todo:改成函数判断当前SELECTED_NODE就绪状态
    int p2p_streaming_ready;

    // P2P 服务器补RTP帧相关
    char *repair_rtp_pkt_host;  // 如 127.0.0.1
    int   repair_rtp_pkt_port;  // 如 1985
    int64_t repair_http_timeout_ms; // ms
    // char *repair_rtp_pkt_url;   // 由用户输入，如 http://host:port/rtc/v1/repair/?app=live&stream=xx&kind=video&seq=%u

    // 流切换
    P2PRtpDemuxSwitchState sw;             // 切换与缓存状态
    int cdn_started;
} P2PRtpDemuxContext;


static void switch_state_init(P2PRtpDemuxSwitchState *st)
{
    st->active_source = ACTIVE_NONE;
    st->pending_switch = SWITCH_NONE;
    st->pending_pkt_valid = 0;
    memset(&st->pending_pkt, 0, sizeof(st->pending_pkt));
}


static int map_fallback_stream_index(P2PRtpDemuxContext *ctx, int fb_si)
{
    if (!ctx->cdn_fmt_ctx || fb_si < 0 || fb_si >= (int)ctx->cdn_fmt_ctx->nb_streams)
        return -1;
    enum AVMediaType t = ctx->cdn_fmt_ctx->streams[fb_si]->codecpar->codec_type;
    if (t == AVMEDIA_TYPE_VIDEO)
        return ctx->output_video_stream_index;
    if (t == AVMEDIA_TYPE_AUDIO)
        return ctx->output_audio_stream_index;
    return -1; // 其他流忽略
}

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
static void p2p_set_receiver_state(P2PRtpDemuxContext* ctx, ReceiverState new_state);
static int p2p_rtp_demux_signal_handler(P2PContext* ctx, const P2PSignalMessage* msg);
static int p2p_join_room_as_receiver(P2PContext* p2p_ctx, const char* room_id);
static int p2p_pending_for_probing_node(P2PRtpDemuxContext* ctx);
static void on_pc_connected_callback(P2PContext* ctx, PeerConnectionNode* node);

// Fallback 相关
static int create_cdn_streams(P2PRtpDemuxContext *ctx);
static int should_switch_to_cdn(P2PRtpDemuxContext *ctx);

// 工具函数
static char *parse_cdn_params(const char *url, const char *key);
static void scan_h264_nals(const uint8_t *data, int size, int *has_sps, int *has_pps, int *has_idr);
static int has_keyframe_info(AVFormatContext *avctx, AVStream *st, const AVPacket *pkt);


// demux类
static int read_one_from_cdn(P2PRtpDemuxContext *ctx, AVPacket *pkt);

//Todo:目前主动获取的是Video,需要写一个逻辑单独主动获取Audio
static int read_one_from_p2p(P2PRtpDemuxContext *ctx, AVPacket *pkt, int *output_stream_index, AVRational *src_tb);

static int try_prepare_switch_to_p2p(P2PRtpDemuxContext *ctx);

static int try_prepare_switch_to_cdn(P2PRtpDemuxContext *ctx);

static int commit_pending_switch_if_ready(P2PRtpDemuxContext *ctx, AVPacket *out);

// ==================== FF定义函数 ====================

static void *p2p_connect_thread_fn(void *arg)
{
    AVFormatContext *avctx = (AVFormatContext *)arg;
    P2PRtpDemuxContext *ctx = avctx->priv_data;
    int ret;

    ret = p2p_pending_for_probing_node(ctx);
    if (ret == 0) {
        ctx->p2p_connect_ready = 1;
        av_log(avctx, AV_LOG_INFO, "P2P background connect reached STREAMING\n");
    } else {
        av_log(avctx, AV_LOG_WARNING, "P2P background connect failed: %s\n", av_err2str(ret));
    }
    ctx->p2p_connect_thread_active = 0;
    return NULL;
}

static int p2p_rtp_demux_init(AVFormatContext *avctx) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;
    P2PContext* p2p_ctx = &ctx->p2p_context;
    int ret;

    // 0. P2P、CDN相关状态初始化
    p2p_ctx->avctx = avctx;

    ctx->cdn_video_stream_index = -1;
    ctx->cdn_audio_stream_index = -1;
    ctx->output_video_stream_index = -1;
    ctx->output_audio_stream_index = -1;
    ctx->cdn_active = 0;
    ctx->cdn_started = 0;
    switch_state_init(&ctx->sw);

    ctx->p2p_connect_thread_active = 0;
    if (!ctx->p2p_connect_thread_active) {
        ctx->p2p_connect_thread_active = 1;
        pthread_create(&ctx->p2p_connect_thread, NULL, p2p_connect_thread_fn, avctx);
        pthread_detach(ctx->p2p_connect_thread);
    }
    p2p_set_receiver_state(ctx, RECEIVER_STATE_JOINING_ROOM);

    ctx->last_p2p_packet_time = av_gettime_relative();
    if (!ctx->room) {
        // 若未显式传入room，从CDN URL中提取stream=后面的参数作为缺省room，加入信令服务器的房间
        char *v = parse_cdn_params(avctx->url, "stream");
        if (v) ctx->room = v;
    }


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
    const char* room_id = ctx->room ? ctx->room : "default_room";
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

    if ((ret = p2p_rtp_demux_init(avctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to p2p_rtp_demux_init\n");
        goto fail;
    }

    // 默认ACTIVE_NONE，等待P2P
    // CDN作为try，如果能成功打开并创建流，则直接使用CDN
    ctx->cdn_url = avctx->url;
    ret = create_cdn_streams(ctx);
    if (ret == 0) {
        ret = try_prepare_switch_to_cdn(ctx);
        if (ret == 0) {
            ctx->cdn_active = 1;
            ctx->sw.active_source = ACTIVE_CDN;
            av_log(avctx, AV_LOG_INFO, "Header: start from CDN as primary source\n");
        } else {
            av_log(avctx, AV_LOG_WARNING, "try_prepare_switch_to_cdn failed (%s)\n", av_err2str(ret));
        }
    } else {
        av_log(avctx, AV_LOG_WARNING, "create_cdn_streams failed (%s)\n", av_err2str(ret));
        ctx->cdn_active = 0;
    }

    return 0;

fail:
    return ret;
}

static int p2p_rtp_read_packet(AVFormatContext *avctx, AVPacket *pkt) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;

    // 若有待切换且缓存帧已就绪，优先输出并完成切换
    if (commit_pending_switch_if_ready(ctx, pkt) == 1)
        return 0;

    int ret;
    // 若还未选择输出源，优先选择已就绪的CDN，否则等待P2P就绪
    if (ctx->sw.active_source == ACTIVE_NONE) {
        if (ctx->p2p_connect_ready && ctx->p2p_context.selected_node) {
            if (ret = try_prepare_switch_to_p2p(ctx) > 0) {
                return ret;
            }
        }
        return AVERROR(EAGAIN);
    } else if (ctx->sw.active_source == ACTIVE_CDN) {
        ret = read_one_from_cdn(ctx, pkt);

        if (ctx->p2p_connect_ready && ctx->p2p_context.selected_node) {
            if (ctx->sw.pending_switch != SWITCH_TO_P2P)
                ctx->sw.pending_switch = SWITCH_TO_P2P;
            try_prepare_switch_to_p2p(ctx);
            if (commit_pending_switch_if_ready(ctx, pkt) == 1)
                return 0;
        }

        return ret;
    } else if (ctx->sw.active_source == ACTIVE_P2P) {
        int output_stream_index = -1;
        AVRational src_tb = (AVRational){1,1};
        ret = read_one_from_p2p(ctx, pkt, &output_stream_index, &src_tb);
        if (ret >= 0) {
            AVRational dst_tb = avctx->streams[output_stream_index]->time_base;
            av_packet_rescale_ts(pkt, src_tb, dst_tb);
            pkt->stream_index = output_stream_index;
            ctx->last_p2p_packet_time = av_gettime_relative();
            ctx->p2p_transport_error_times = 0;
            return 0;
        } else {
            ++ctx->p2p_transport_error_times;
        }

        if (ctx->p2p_transport_error_times > 100) {
            av_log(avctx, AV_LOG_ERROR, "%d");
            if (ctx->sw.pending_switch != SWITCH_TO_CDN)
                ctx->sw.pending_switch = SWITCH_TO_CDN;
            if (!ctx->cdn_fmt_ctx && ctx->cdn_url)
                create_cdn_streams(ctx);
            if (ctx->cdn_fmt_ctx) {
                try_prepare_switch_to_cdn(ctx);
                if (commit_pending_switch_if_ready(ctx, pkt) == 1)
                    return 0;
            }
        }
        return ret;
    }

    return AVERROR(EAGAIN);
}

static int p2p_rtp_close(AVFormatContext *avctx) {
    P2PRtpDemuxContext *ctx = avctx->priv_data;

    av_log(avctx, AV_LOG_INFO, "Closing P2P RTP demux\n");

    p2p_close_resource(&ctx->p2p_context);

    if (ctx->cdn_fmt_ctx) {
        avformat_close_input(&ctx->cdn_fmt_ctx);
        ctx->cdn_fmt_ctx = NULL;
    }

    if (ctx->room) {
        av_freep(&ctx->room);
    }

    if (ctx->p2p_context.signal_callbacks) {
        av_freep(&ctx->p2p_context.signal_callbacks);
    }

    if (ctx->sw.pending_pkt_valid) {
        av_packet_unref(&ctx->sw.pending_pkt);
        ctx->sw.pending_pkt_valid = 0;
    }

    return 0;
}

// ==================== 状态与回调 ====================
static void p2p_set_receiver_state(P2PRtpDemuxContext* ctx, ReceiverState new_state) {
    if (ctx->current_state < new_state) {
        av_log(ctx->p2p_context.avctx, AV_LOG_INFO,
               "Receiver state change: %s -> %s\n", get_receiver_state_name(ctx->current_state), get_receiver_state_name(new_state));
        ctx->current_state = new_state;
        ctx->state_enter_time = av_gettime_relative();
    }
}

static void on_pc_connected_callback(P2PContext* ctx, PeerConnectionNode* node) {
    P2PRtpDemuxContext* demux_ctx = (P2PRtpDemuxContext*)ctx->avctx->priv_data;

    av_log(ctx->avctx, AV_LOG_INFO, "P2P connection established with sender %s, starting network probing\n", node->remote_id);

    probe_data_init(&node->network_quality);

    p2p_set_receiver_state(demux_ctx, RECEIVER_STATE_DATACHANNEL_READY);

    av_log(ctx->avctx, AV_LOG_INFO, "Receiver state set to DATACHANNEL_READY, will send probe request to %s\n", node->remote_id);
}

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

                p2p_set_receiver_state(demux_ctx, RECEIVER_STATE_WAITING_FOR_SENDER);
            }
            break;

        case P2P_MSG_CONNECT_REQUEST:
            {
                const char* target_id = msg->payload.connect_request.target_id;
                av_log(ctx->avctx, AV_LOG_INFO, "Received connect request to %s\n", target_id);

                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, target_id);
                if (node == NULL) {
                    ret = create_peer_connection(ctx, remote_id);
                    if (ret < 0) {
                        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create peer connection for %s\n", target_id);
                        return ret;
                    }

                    node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, target_id);
                }

                init_peer_connection(ctx, node->pc_id);
                p2p_set_receiver_state(demux_ctx, RECEIVER_STATE_SIGNALING);
            }
            break;

        case P2P_MSG_OFFER:
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node == NULL) {
                    ret = create_peer_connection(ctx, remote_id);
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
                    p2p_set_receiver_state(demux_ctx, RECEIVER_STATE_SIGNALING);
                }
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
        /*
        case P2P_MSG_PROBE_RESPONSE:
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
                if (node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Received probe response from %s, probe_id=%s, status=%s\n",
                           remote_id, msg->payload.probe_response.probe_id, msg->payload.probe_response.status);

                    if (strcmp(msg->payload.probe_response.status, "ok") == 0) {
                        // p2p_set_receiver_state(demux_ctx, RECEIVER_STATE_PROBE_COMPLETED);
                    }
                }
            }
            break;
        */
        case P2P_MSG_STREAM_READY:
            {
                PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);

                if (node && node == ctx->selected_node) {
                    av_log(ctx->avctx, AV_LOG_INFO, "Stream ready from %s, stream_id=%s\n",
                           remote_id, msg->payload.stream_ready.stream_id ? msg->payload.stream_ready.stream_id : "unknown");

                    p2p_set_receiver_state(demux_ctx, RECEIVER_STATE_STREAMING);
                } else {
                    av_log(ctx->avctx, AV_LOG_WARNING, "Received stream ready from non-selected node %s\n", remote_id);
                }
            }
            break;

        case P2P_MSG_ERROR:
            {
                av_log(ctx->avctx, AV_LOG_ERROR, "Received error from %s: code=%s, msg=%s\n",
                       remote_id ? remote_id : "unknown",
                       msg->payload.error.error_code ? msg->payload.error.error_code : "unknown",
                       msg->payload.error.error_msg ? msg->payload.error.error_msg : "unknown");
                p2p_set_receiver_state(demux_ctx, RECEIVER_STATE_ERROR);
            }
            break;

        default:
            av_log(ctx->avctx, AV_LOG_DEBUG, "Unhandled message type: %d\n", msg->type);
            break;
    }

    return 0;
}

static int p2p_pending_for_probing_node(P2PRtpDemuxContext* ctx) {
    int64_t timeout_us = ctx->wait_timeout * 1000000LL;
    int64_t start_time = av_gettime_relative();

    while (ctx->current_state != RECEIVER_STATE_STREAMING) {
        if (av_gettime_relative() - start_time > timeout_us) {
            av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "Timeout waiting for streaming state after %d seconds\n",
                   ctx->wait_timeout ? ctx->wait_timeout : P2P_WAITING_SENDER_TIMEOUT_SEC);
            return AVERROR(ETIMEDOUT);
        }

        if (ctx->current_state == RECEIVER_STATE_ERROR) {
            av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "Receiver entered error state\n");
            return AVERROR_EXTERNAL;
        }

        switch (ctx->current_state) {
            case RECEIVER_STATE_DATACHANNEL_READY:
                // DataChannel就绪后，初始化所有节点的探测结构并发送探测请求
                {
                    int connected_nodes = 0;
                    int probe_sent = 0;

                    // 1.初始化所有已连接节点的探测结构
                    PeerConnectionNode* curr = ctx->p2p_context.peer_connection_node_caches;
                    while (curr) {
                        if (curr->status == Connected) {
                            connected_nodes++;
                            probe_data_init(&curr->network_quality);
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
                        av_usleep(100 * 1000); // 100ms
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
                    p2p_set_receiver_state(ctx, RECEIVER_STATE_PROBING);
                }
                break;

            case RECEIVER_STATE_PROBING:
                {
                    int64_t probe_timeout = 30 * 1000000LL;
                    int64_t probe_start_time = ctx->state_enter_time;

                    av_log(ctx->p2p_context.avctx, AV_LOG_INFO,
                           "Entering probe pending mode, will check every 1 second\n");

                    while (ctx->current_state == RECEIVER_STATE_PROBING) {
                        int total_nodes = 0;
                        int completed_nodes = 0;
                        int64_t current_time = av_gettime_relative();
                        int64_t elapsed_since_probe_start = current_time - probe_start_time;

                        if (elapsed_since_probe_start > probe_timeout) {
                            av_log(ctx->p2p_context.avctx, AV_LOG_WARNING,
                                   "Probe phase timeout after %lld seconds, forcing completion\n",
                                   elapsed_since_probe_start / 1000000);

                            PeerConnectionNode* curr = ctx->p2p_context.peer_connection_node_caches;
                            while (curr) {
                                if (curr->status == Connected && curr->network_quality.phase != PROBING_COMPLETE) {
                                    curr->network_quality.phase = PROBING_COMPLETE;
                                    av_log(ctx->p2p_context.avctx, AV_LOG_WARNING,
                                           "Force completing probe for node %s due to timeout\n", curr->remote_id);
                                }
                                curr = curr->next;
                            }
                            break;
                        }

                        PeerConnectionNode* curr = ctx->p2p_context.peer_connection_node_caches;
                        while (curr) {
                            if (curr->status == Connected) {
                                total_nodes++;

                                if (curr->network_quality.phase == PROBING_COMPLETE) {
                                    completed_nodes++;
                                    av_log(ctx->p2p_context.avctx, AV_LOG_DEBUG,
                                           "Node %s: probe completed\n", curr->remote_id);
                                } else {
                                    if (curr->network_quality.timeout_ms > 0 &&
                                        curr->network_quality.probe_start_time > 0) {
                                        int64_t node_elapsed_ms = (current_time - curr->network_quality.probe_start_time) / 1000;
                                        if (node_elapsed_ms >= curr->network_quality.timeout_ms) {
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

                        if (total_nodes == 0) {
                            av_log(ctx->p2p_context.avctx, AV_LOG_ERROR,
                                   "No connected nodes found during probing\n");
                            p2p_set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                            return AVERROR(EINVAL);
                        } else if (completed_nodes >= total_nodes) {
                            av_log(ctx->p2p_context.avctx, AV_LOG_INFO,
                                   "All %d nodes completed probing, exiting probe pending mode\n", total_nodes);
                            break;
                        } else {
                            av_log(ctx->p2p_context.avctx, AV_LOG_DEBUG,
                                   "Probe pending: %d/%d completed, sleeping 1s for next check\n",
                                   completed_nodes, total_nodes);
                            av_usleep(100 * 1000); // 100ms
                        }
                    }

                    if (ctx->current_state == RECEIVER_STATE_PROBING) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_INFO,
                               "Probe pending completed, moving to selection phase\n");
                        p2p_set_receiver_state(ctx, RECEIVER_STATE_PROBE_COMPLETED);
                    }
                }
                break;

            case RECEIVER_STATE_PROBE_COMPLETED:
                {
                    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, "Starting best node selection process\n");

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
                        p2p_set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return AVERROR(EINVAL);
                    }

                    PeerConnectionNode* best_node = select_best_node(&ctx->p2p_context);
                    if (!best_node) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR,
                               "Failed to select best node from %d available nodes\n", available_nodes);
                        p2p_set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return AVERROR(EINVAL);
                    }

                    av_log(ctx->p2p_context.avctx, AV_LOG_INFO,
                           "Selected best node: %s (score=%.2f) from %d candidates\n",
                           best_node->remote_id, best_node->network_quality.final_score, available_nodes);

                    ctx->p2p_context.selected_node = best_node;
                    best_node->status = Selected;

                    p2p_set_receiver_state(ctx, RECEIVER_STATE_REQUESTING_STREAM);
                }
                break;

            case RECEIVER_STATE_REQUESTING_STREAM:
                {
                    if (!ctx->p2p_context.selected_node) {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR, "No selected node for stream request\n");
                        p2p_set_receiver_state(ctx, RECEIVER_STATE_ERROR);
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
                    } else {
                        av_log(ctx->p2p_context.avctx, AV_LOG_ERROR,
                               "Failed to send stream request to %s: %s\n",
                               ctx->p2p_context.selected_node->remote_id, av_err2str(ret));
                        p2p_set_receiver_state(ctx, RECEIVER_STATE_ERROR);
                        return ret;
                    }

                    av_usleep(100 * 1000); // 100ms
                }
                break;

            default:
                av_usleep(100 * 1000); // 100ms
                break;
        }
    }

    av_log(ctx->p2p_context.avctx, AV_LOG_INFO, "Successfully reached streaming state\n");
    return 0;
}

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

    // Todo: Feature: 添加推拉流端能力检测
    msg->payload.join_room.capabilities = strdup("video:h264,h265;audio:aac,opus;max_bitrate:10000kbps");

    int ret = p2p_send_signal_message(p2p_ctx, msg);

    av_log(p2p_ctx->avctx, AV_LOG_INFO, "Joining room as receiver: %s\n", room_id);

    p2p_free_signal_message(msg);
    return ret;
}


#define OFFSET(x) offsetof(P2PRtpDemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    { "room", "Room to join (same as CDN stream)", OFFSET(room), AV_OPT_TYPE_STRING, { .str = "default_room" }, 0, 0, DEC },
    { "wait_timeout", "Timeout for waiting streaming (seconds)", OFFSET(wait_timeout), AV_OPT_TYPE_INT, { .i64 = P2P_WAITING_SENDER_TIMEOUT_SEC }, 5, 300, DEC },
    // { "enable_fallback", "Enable automatic fallback to WHEP when P2P stalls or disconnects", OFFSET(enable_fallback), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, DEC },
    { "stall_switch_ms", "No-packet duration before switching to fallback (ms)", OFFSET(stall_switch_ms), AV_OPT_TYPE_INT64, { .i64 = 3000 }, 500, 60000, DEC },
    { "repair_rtp_pkt_host", "Repair RTP packet Host", OFFSET(repair_rtp_pkt_host), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, DEC },
    { "repair_rtp_pkt_port", "Repair RTP packet Port", OFFSET(repair_rtp_pkt_port), AV_OPT_TYPE_INT, { .i64 = 1985 }, 1, 65535, DEC },
    { "repair_http_timeout_ms", "Repair HTTP timeout (ms)", OFFSET(repair_http_timeout_ms), AV_OPT_TYPE_INT64, { .i64 = 800 }, 100, 60000, DEC },
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

// ==================== 内部辅助实现 ====================

static int read_one_from_cdn(P2PRtpDemuxContext *ctx, AVPacket *pkt)
{
    if (!ctx->cdn_active || !ctx->cdn_fmt_ctx)
        return AVERROR(EAGAIN);

    int ret = av_read_frame(ctx->cdn_fmt_ctx, pkt);
    if (ret < 0)
        return ret;

    int output_stream_index = -1;
    // Todo: Audio/Video stream_index治理
    enum AVMediaType t = ctx->cdn_fmt_ctx->streams[pkt->stream_index]->codecpar->codec_type;
    if (t == AVMEDIA_TYPE_VIDEO)
        output_stream_index = ctx->output_video_stream_index;
    else if (t == AVMEDIA_TYPE_AUDIO)
        output_stream_index = ctx->output_audio_stream_index;

    if (output_stream_index < 0 || output_stream_index >= (int)ctx->p2p_context.avctx->nb_streams) {
        av_packet_unref(pkt);
        return AVERROR(EAGAIN);
    }

    if (!ctx->cdn_started && output_stream_index == ctx->cdn_video_stream_index) {
        if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) {
            av_packet_unref(pkt);
            return AVERROR(EAGAIN);
        }
        ctx->cdn_started = 1;
    }

    AVRational src_tb = ctx->cdn_fmt_ctx->streams[pkt->stream_index]->time_base;
    AVRational dst_tb = ctx->p2p_context.avctx->streams[output_stream_index]->time_base;
    av_packet_rescale_ts(pkt, src_tb, dst_tb);

    pkt->stream_index = output_stream_index;
    return 0;
}

//Todo:目前主动获取的是Video,需要写一个逻辑单独主动获取Audio
static int read_one_from_p2p(P2PRtpDemuxContext *ctx, AVPacket *pkt, int *output_stream_index, AVRational *src_tb)
{
    PeerConnectionTrack *track = ctx->p2p_context.selected_node->video_track;
    if (!track || !track->rtp_ctx)
        return AVERROR(EAGAIN);

    int ret = av_read_frame(track->rtp_ctx, pkt);
    if (ret < 0)
        return ret;

    *output_stream_index = ctx->output_video_stream_index; // xy:Todo: fix video stream index
    *src_tb = track->rtp_ctx->streams[pkt->stream_index]->time_base;
    return 0;
}

static int try_prepare_switch_to_p2p(P2PRtpDemuxContext *ctx)
{
    if (!ctx->p2p_connect_ready || !ctx->p2p_context.selected_node)
        return AVERROR(EINVAL);
    if (ctx->sw.pending_switch == SWITCH_TO_P2P && ctx->sw.pending_pkt_valid)
        return AVERROR(EINVAL);

    for (int i = 0; i < P2P_DEFAULT_VIDEO_GOP; i++) {
        AVPacket tmp;
        av_init_packet(&tmp);
        int output_stream_index = -1;
        AVRational src_tb = (AVRational){1,1};
        int r = read_one_from_p2p(ctx, &tmp, &output_stream_index, &src_tb);
        if (r < 0) {
            if (r != AVERROR(EAGAIN))
                av_packet_unref(&tmp);
            break;
        }
        PeerConnectionTrack *track = ctx->p2p_context.selected_node->video_track;
        AVStream *p2p_st = NULL;
        if (track && track->rtp_ctx && tmp.stream_index < (int)track->rtp_ctx->nb_streams) {
            p2p_st = track->rtp_ctx->streams[tmp.stream_index];
        } else {
            continue;
        }

        int suitable = has_keyframe_info(ctx->p2p_context.avctx, p2p_st, &tmp);

        if (suitable) {
            if (ctx->output_video_stream_index == -1) {
                ctx->output_video_stream_index = track->stream_index;
            }
            // audio待切换
            if (av_packet_ref(&ctx->sw.pending_pkt, &tmp) == 0) {
                ctx->sw.pending_pkt_valid = 1;
                ctx->sw.pending_pkt_from = ACTIVE_P2P;
                ctx->sw.pending_pkt_out_stream_index = ctx->output_video_stream_index;
                ctx->sw.pending_pkt_src_tb = src_tb;
                ctx->sw.pending_switch = SWITCH_TO_P2P;
                av_packet_unref(&tmp);
                break;
            }
        }
        av_packet_unref(&tmp);
    }
    return (ctx->sw.pending_pkt_valid == 1 && ctx->sw.pending_switch == SWITCH_TO_P2P)? 0: AVERROR(EAGAIN);
}

// Todo:兼容长Gop视频，后续应从服务器获取
static int try_prepare_switch_to_cdn(P2PRtpDemuxContext *ctx) {
    if (!ctx->cdn_fmt_ctx)
        return AVERROR(EINVAL);
    if (ctx->sw.pending_switch == SWITCH_TO_CDN && ctx->sw.pending_pkt_valid)
        return AVERROR(EINVAL);

    for (int i = 0; i < P2P_DEFAULT_VIDEO_GOP; i++) {
        AVPacket tmp; av_init_packet(&tmp);
        int r = av_read_frame(ctx->cdn_fmt_ctx, &tmp);
        if (r < 0) {
            if (r != AVERROR(EAGAIN))
                av_packet_unref(&tmp);
            break;
        }
        AVStream *fb_st = ctx->cdn_fmt_ctx->streams[tmp.stream_index];
        if (fb_st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            av_packet_unref(&tmp);
            continue;
        }
        int suitable = has_keyframe_info(ctx->p2p_context.avctx, fb_st, &tmp);
        if (suitable) {
            if (ctx->output_video_stream_index == -1) {
                ctx->output_video_stream_index = ctx->cdn_video_stream_index;
            }
            AVRational src_tb = fb_st->time_base;
            if (av_packet_ref(&ctx->sw.pending_pkt, &tmp) == 0) {
                ctx->sw.pending_pkt_valid = 1;
                ctx->sw.pending_pkt_from = ACTIVE_CDN;
                ctx->sw.pending_pkt_out_stream_index = ctx->output_video_stream_index;
                ctx->sw.pending_pkt_src_tb = src_tb;
                ctx->sw.pending_switch = SWITCH_TO_CDN;
                av_packet_unref(&tmp);
                break;
            }
        }
        av_packet_unref(&tmp);
    }
    return (ctx->sw.pending_pkt_valid == 1 && ctx->sw.pending_switch == SWITCH_TO_CDN)? 0: AVERROR(EAGAIN);
}

static int commit_pending_switch_if_ready(P2PRtpDemuxContext *ctx, AVPacket *out)
{
    if (!ctx->sw.pending_pkt_valid)
        return 0;

    int out_index = ctx->sw.pending_pkt_out_stream_index;
    ctx->sw.pending_pkt.stream_index = out_index;

    if (ctx->sw.pending_switch == SWITCH_TO_P2P) {
        ctx->sw.active_source = ACTIVE_P2P;
    }
    else if (ctx->sw.pending_switch == SWITCH_TO_CDN) {
        ctx->sw.active_source = ACTIVE_CDN;
    }
    ctx->sw.pending_switch = SWITCH_NONE;

    ctx->cdn_active = (ctx->sw.active_source == ACTIVE_CDN);
    ctx->sw.pending_pkt_valid = 0;

    // out packet
    AVRational dst_tb = ctx->p2p_context.avctx->streams[out_index]->time_base;
    av_packet_rescale_ts(&ctx->sw.pending_pkt, ctx->sw.pending_pkt_src_tb, dst_tb);
    int ret = av_packet_ref(out, &ctx->sw.pending_pkt);
    av_packet_unref(&ctx->sw.pending_pkt);

    return ret == 0 ? 1 : 0;
}

static int create_cdn_streams(P2PRtpDemuxContext *ctx)
{
    AVFormatContext *avctx = ctx->p2p_context.avctx;

    if (ctx->cdn_fmt_ctx && ctx->cdn_active)
        return 0;

    const AVInputFormat *infmt = av_find_input_format("whep");
    if (!infmt)
        return AVERROR_DEMUXER_NOT_FOUND;

    AVDictionary *opts = NULL;
    // 直接使用上层传进来的 URL（avctx->url），但此处拿不到 avctx；调用方保证我们在 read_header 中使用
    // 因此在 read_header 中调用本函数前，先把 URL 传入本地静态缓存
    if (!ctx->cdn_url || !ctx->cdn_url[0])
        return AVERROR(EINVAL);
    int ret = avformat_open_input(&ctx->cdn_fmt_ctx, ctx->cdn_url, infmt, &opts);
    if (ret < 0 )
        return ret;
    if (!ctx->cdn_fmt_ctx)
        return AVERROR(EINVAL);
    
    // Todo:这个过程会probe一段时间，后续可在服务器添加定制接口，优化该过程
    ret = avformat_find_stream_info(ctx->cdn_fmt_ctx, NULL);
    if (ret < 0) {
        avformat_close_input(&ctx->cdn_fmt_ctx);
        return ret;
    }

    // 这里是avctx不是一个，是嵌套关系，所以需要创建stream
    for (unsigned i = 0; i < ctx->cdn_fmt_ctx->nb_streams; i++) {
        AVStream *src = ctx->cdn_fmt_ctx->streams[i];
        enum AVMediaType t = src->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        AVStream *st = avformat_new_stream(avctx, NULL);
        if (!st) {
            return AVERROR(ENOMEM);
        }
        int ret = avcodec_parameters_copy(st->codecpar, src->codecpar);
        if (ret < 0) {
            return ret;
        }
        st->time_base = src->time_base;
        if (t == AVMEDIA_TYPE_VIDEO && ctx->cdn_video_stream_index < 0) {
            ctx->cdn_video_stream_index = st->index;
            if (ctx->output_video_stream_index == -1) {
                ctx->output_video_stream_index = st->index;
            }
        }
        if (t == AVMEDIA_TYPE_AUDIO && ctx->cdn_audio_stream_index < 0) {
            ctx->cdn_audio_stream_index = st->index;
            if (ctx->output_audio_stream_index == -1) {
                ctx->output_audio_stream_index = st->index;
            }
        }
        if (ctx->cdn_video_stream_index >= 0 && ctx->cdn_audio_stream_index >= 0) {
            break;
        }
    }
    
    return avctx->nb_streams > 0 ? 0 : AVERROR(EINVAL);
}

// ==================== 工具函数 ====================

// 简易 H264 NAL 扫描（支持 Annex-B 与长度前缀两种常见格式）
static void scan_h264_nals(const uint8_t *data, int size, int *has_sps, int *has_pps, int *has_idr)
{
    *has_sps = 0; *has_pps = 0; *has_idr = 0;
    if (!data || size <= 0) return;

    int found_startcode = 0;
    // Annex-B 探测
    for (int i = 0; i + 4 <= size; i++) {
        int sc3 = i + 3 <= size && data[i] == 0 && data[i+1] == 0 && data[i+2] == 1;
        int sc4 = i + 4 <= size && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1;
        if (!sc3 && !sc4) continue;
        found_startcode = 1;
        int nal_start = i + (sc4 ? 4 : 3);
        // 找下一起始码
        int j = nal_start;
        for (; j + 4 <= size; j++) {
            int nsc3 = data[j] == 0 && data[j+1] == 0 && data[j+2] == 1;
            int nsc4 = j + 1 < size && data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1;
            if (nsc3 || nsc4) break;
        }
        if (nal_start < size) {
            uint8_t nalh = data[nal_start];
            int nal_type = nalh & 0x1F;
            if (nal_type == 7) *has_sps = 1;
            else if (nal_type == 8) *has_pps = 1;
            else if (nal_type == 5) *has_idr = 1;
        }
        i = j - 1;
    }

    if (!found_startcode) {
        // 可能是长度前缀模式，尝试解析
        int pos = 0; int ok = 0;
        while (pos + 4 <= size) {
            uint32_t len = AV_RB32(data + pos);
            pos += 4;
            if (len == 0 || pos + (int)len > size) { ok = 0; break; }
            ok = 1;
            uint8_t nalh = data[pos];
            int nal_type = nalh & 0x1F;
            if (nal_type == 7) *has_sps = 1;
            else if (nal_type == 8) *has_pps = 1;
            else if (nal_type == 5) *has_idr = 1;
            pos += (int)len;
        }
        (void)ok;
    }
}

static int has_keyframe_info(AVFormatContext *avctx, AVStream *st, const AVPacket *pkt)
{
    if (!st || !st->codecpar ||
        !pkt || !pkt->data || pkt->size <= 0) {
        return 0;
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_H264) {
        int has_idr = 0, has_sps = 0, has_pps = 0;
        scan_h264_nals(pkt->data, pkt->size, &has_sps, &has_pps, &has_idr);
        if (!has_idr) {
            return 0;
        }

        if (st->codecpar && st->codecpar->extradata && st->codecpar->extradata_size > 0) {
            // 若stream中已有 global extradata info（SPS/PPS）
            return 1;
        } else {
            return has_sps && has_pps;
        }
    }

    return (pkt->flags & AV_PKT_FLAG_KEY);
}

static char *parse_cdn_params(const char *url, const char *key)
{
    if (!url || !key) return NULL;
    const char *q = strchr(url, '?');
    if (!q) return NULL;
    q++;
    size_t keylen = strlen(key);
    while (*q) {
        const char *eq = strchr(q, '=');
        const char *amp = strchr(q, '&');
        if (!eq) break;
        if (!amp) amp = q + strlen(q);
        if ((size_t)(eq - q) == keylen && !av_strncasecmp(q, key, keylen)) {
            size_t vlen = (size_t)(amp - (eq + 1));
            char *val = av_malloc(vlen + 1);
            if (!val) return NULL;
            memcpy(val, eq + 1, vlen);
            val[vlen] = '\0';
            return val;
        }
        if (!*amp) break;
        q = amp + 1;
    }
    return NULL;
}