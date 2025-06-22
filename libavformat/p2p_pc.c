//
// Created by Shaw on 2025/3/4.
//
#include "p2p_pc.h"
#include "p2p_dc.h"
#include "p2p_ws.h"
#include <unistd.h>
#include "rtpenc.h"
#include "webrtc.h"
#include "rtsp.h"
#include <libavutil/time.h>


int p2p_init_signal_server(P2PContext* const ctx) {
    rtcConfiguration* config = malloc(sizeof(rtcConfiguration));
    memset(config, 0, sizeof(config));
    char* iceServers[] = {
        "stun:stun.l.google.com:19302",
        NULL
    };
    config->iceServersCount = sizeof(iceServers) / sizeof(iceServers[0]);
    config->iceServers = (char**)malloc(config->iceServersCount * sizeof(char*));
    if (config->iceServers == NULL) {
    }
    for (int i = 0; i < config->iceServersCount; i++) {
        config->iceServers[i] = strdup(iceServers[i]);
        if (config->iceServers[i] == NULL) {
            abort();
        }
    }


    ctx->config = config;

    const char* web_socket_server_address = "120.53.223.132";
    const char* web_socket_server_port = "8000";
    init_ws_resource(ctx, web_socket_server_address, web_socket_server_port);

    //我方主动建联的测试
    while(ctx->web_socket_connected == 0) {
        av_usleep(1000); //等待ws连接成功 //xy:ToDo:Temp: 应该改成一个阻塞机制，可以放到回调函数里，设置超时时间。
    }
    return 0;
}

int init_peer_connection(P2PContext* const ctx, const char* remote_id) {
    if (ctx == NULL) {
        return AVERROR_INVALIDDATA;
    }
    int ret;
    int peer_connection;
    rtcConfiguration* config = ctx->config;

    if ((peer_connection = rtcCreatePeerConnection(config)) <= 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create PeerConnection\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    PeerConnectionNode* pc_node = av_mallocz(sizeof(PeerConnectionNode));
    if (pc_node == NULL) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate PeerConnectionNode\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    pc_node->avctx = ctx->avctx;
    pc_node->status = Disconnected;     //xy:ToDo: 3期实现NetWorkTest状态
    pc_node->pc_id = peer_connection;
    av_log(ctx->avctx, AV_LOG_INFO, "init local pc id: %d\n", peer_connection);
    pc_node->remote_id = strdup(remote_id); //xy:todo:free
    pc_node->p2p_ctx = ctx;
    append_peer_connection_node_to_list(&ctx->peer_connection_node_caches, pc_node);

    rtcSetUserPointer(peer_connection, pc_node);
    // only pc
    ret = rtcSetLocalDescriptionCallback(peer_connection, on_pc_local_description_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetLocalDescriptionCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetLocalCandidateCallback(peer_connection, on_pc_local_candidate_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetLocalCandidateCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetStateChangeCallback(peer_connection, on_pc_state_change_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetStateChangeCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetIceStateChangeCallback(peer_connection, on_pc_ice_state_change_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetIceStateChangeCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetGatheringStateChangeCallback(peer_connection, on_pc_gathering_state_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetGatheringStateChangeCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetSignalingStateChangeCallback(peer_connection, on_pc_signaling_state_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetSignalingStateChangeCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    // pc with dc
    ret = rtcSetDataChannelCallback(peer_connection, on_pc_data_channel_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetDataChannelCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetTrackCallback(peer_connection, on_pc_track_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetTrackCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }


    return 0;
fail:
    rtcDeletePeerConnection(peer_connection);
    abort();
    return ret;
}

// -------- peer connect callback(pc only) --------
//这代表我方主动建连
void on_pc_local_description_callback(int peer_connection_id, const char *sdp, const char *type, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;
    int ret;

    av_log(ctx->avctx, AV_LOG_DEBUG, "peer_connection(local id: %d with remote id: %s) local description set (type=%s)\n", peer_connection_id, node->remote_id, type);

    cJSON* message = cJSON_CreateObject();
    if (message == NULL) {
        fprintf(stderr, "无法创建 JSON 对象\n");
        return;
    }

    cJSON_AddStringToObject(message, "id", node->remote_id);
    cJSON_AddStringToObject(message, "type", type);
    cJSON_AddStringToObject(message, "description", sdp);

    // 将 JSON 对象转换为紧凑字符串
    char* json_str = cJSON_PrintUnformatted(message);
    if (json_str == NULL) {
        fprintf(stderr, "无法生成 JSON 字符串\n");
        cJSON_Delete(message);
        return;
    }
    size_t size = strlen(json_str);
    // 通过 WebSocket 发送 JSON 数据
    ret = rtcSendMessage(ctx->web_socket, json_str, size);
    if (ret < 0) {
        // av_log();
    }
end:
    free(json_str);
    cJSON_Delete(message);
}

void on_pc_local_candidate_callback(int peer_connection_id, const char *cand, const char *mid, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_DEBUG, "peer_connection(local id: %d with remote id: %s) local ICE candidate (mid=%s)\n", peer_connection_id, node->remote_id, mid);

    cJSON* message = cJSON_CreateObject();
    if (message == NULL) {
        fprintf(stderr, "无法创建 JSON 对象\n");
        return;
    }

    cJSON_AddStringToObject(message, "id", node->remote_id);
    cJSON_AddStringToObject(message, "type", "candidate");
    cJSON_AddStringToObject(message, "candidate", cand);
    cJSON_AddStringToObject(message, "mid", mid);

    char* json_str = cJSON_PrintUnformatted(message);
    if (json_str == NULL) {
        fprintf(stderr, "无法生成 JSON 字符串\n");
        cJSON_Delete(message);
        return;
    }

    size_t size = strlen(json_str);
    rtcSendMessage(ctx->web_socket, json_str, size);

end:
    free(json_str);
    cJSON_Delete(message);
}

static const char* webrtc_get_state_name(const rtcState state)
{
    switch (state)
    {
    case RTC_NEW:
        return "RTC_NEW";
    case RTC_CONNECTING:
        return "RTC_CONNECTING";
    case RTC_CONNECTED:
        return "RTC_CONNECTED";
    case RTC_DISCONNECTED:
        return "RTC_DISCONNECTED";
    case RTC_FAILED:
        return "RTC_FAILED";
    case RTC_CLOSED:
        return "RTC_CLOSED";
    default:
        return "UNKNOWN";
    }
}

static const char* webrtc_get_ice_state_name(const rtcIceState state) {
    switch (state) {
    case RTC_ICE_NEW:
        return "RTC_ICE_NEW";
    case RTC_ICE_CHECKING:
        return "RTC_ICE_CHECKING";
    case RTC_ICE_CONNECTED:
        return "RTC_ICE_CONNECTED";
    case RTC_ICE_COMPLETED:
        return "RTC_ICE_COMPLETED";
    case RTC_ICE_FAILED:
        return "RTC_ICE_FAILED";
    case RTC_ICE_DISCONNECTED:
        return "RTC_ICE_DISCONNECTED";
    case RTC_ICE_CLOSED:
        return "RTC_ICE_CLOSED";
    default:
        return "RTC_ICE_UNKNOWN";
    }
}
static const char* webrtc_get_gathering_state_name(const rtcGatheringState state) {
    switch (state) {
    case RTC_GATHERING_NEW:
        return "RTC_GATHERING_NEW";
    case RTC_GATHERING_INPROGRESS:
        return "RTC_GATHERING_INPROGRESS";
    case RTC_GATHERING_COMPLETE:
        return "RTC_GATHERING_COMPLETE";
    default:
        return "RTC_GATHERING_UNKNOWN";
    }
}
static const char* webrtc_get_signaling_state_name(const rtcSignalingState state)
{
    switch (state) {
    case RTC_SIGNALING_STABLE:
        return "RTC_SIGNALING_STABLE";
    case RTC_SIGNALING_HAVE_LOCAL_OFFER:
        return "RTC_SIGNALING_HAVE_LOCAL_OFFER";
    case RTC_SIGNALING_HAVE_REMOTE_OFFER:
        return "RTC_SIGNALING_HAVE_REMOTE_OFFER";
    case RTC_SIGNALING_HAVE_LOCAL_PRANSWER:
        return "RTC_SIGNALING_HAVE_LOCAL_PRANSWER";
    case RTC_SIGNALING_HAVE_REMOTE_PRANSWER:
        return "RTC_SIGNALING_HAVE_REMOTE_PRANSWER";
    default:
        return "RTC_SIGNALING_UNKNOWN";
    }
}

void on_pc_state_change_callback(int peer_connection_id, rtcState state, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) state changed to %s\n", peer_connection_id, node->remote_id, webrtc_get_state_name(state));

    switch (state) {
    case RTC_NEW:
        node->status = Disconnected;  // 新建状态视为未连接
        break;
    case RTC_CONNECTING:
        node->status = Connecting;    // 直接对应
        break;
    case RTC_CONNECTED:
        node->status = Connected;     // 直接对应
        break;
    case RTC_DISCONNECTED:
        node->status = Disconnected;  // 直接对应
        break;
    case RTC_FAILED:
        node->status = Failed;        // 直接对应
        break;
    case RTC_CLOSED:
        node->status = Completed;     // 关闭视为完成（或Disconnected，根据业务需求）
        break;
    default:
        node->status = Disconnected;  // 默认处理
        break;
    }
    //xy:TODO:optimize: 根据状态进行处理：
    // - 断线重连
    // - 释放资源
    // - 触发上层回调
}

void on_pc_ice_state_change_callback(int peer_connection_id, rtcIceState state, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) ICE state changed to %s\n", peer_connection_id, node->remote_id, webrtc_get_ice_state_name(state));

    //xy:TODO:optimize 根据 ICE 状态进行处理：
    // - 当 ICE 完成时，建立媒体流
    // - 处理 ICE 失败情况（尝试重连或切换网络）
}


void on_pc_gathering_state_callback(int peer_connection_id, rtcGatheringState state, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) gathering state changed to %s\n", peer_connection_id, node->remote_id, webrtc_get_gathering_state_name(state));

    //xy:TODO:optimize 可以在 `Complete` 状态时上报 ICE 候选信息
}

void on_pc_signaling_state_callback(int peer_connection_id, rtcSignalingState state, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) signaling state changed to %s\n", peer_connection_id, node->remote_id, webrtc_get_signaling_state_name(state));

    //xy:TODO:optimize 根据信令状态：
    // - 发送 Offer/Answer
    // - 检查是否需要重新协商（Renegotiation）
}

void on_pc_data_channel_callback(int peer_connection_id, int data_channel_id, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) new data channel established (dc_id=%d)\n", peer_connection_id, node->remote_id, data_channel_id);

    //xy:TODO:optimize 可以初始化数据通道，设置消息回调

    //无论是主动还是被动，前面应该已经调用过init_data_channel了，这里只做一下查询，如果没有再调用一遍初始化
    if (find_peer_connection_track_by_track_id(node->track_caches, data_channel_id) == NULL) {
        printf("data_channel_callback | [warning]⚠️ pc in dc caches is not found！ create new！！！\n");
        // PeerConnectionNode * = find_peer_connection_node_by_pc(ctx->peer_connection_node_caches, peer_connection_id);
        // if(node == NULL)
        // {
        //     assert(false);
        //     return;
        // }
        abort();
        //deprecated
        // init_track(node, node->remote_id);
    } else
    {
        av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) found the channel in caches! (dc_id=%d)\n", peer_connection_id, node->remote_id, data_channel_id);
    }

}

void on_pc_track_callback(int peer_connection_id, int tr, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *p2p_ctx = node->p2p_ctx;
    rtcTrackInit track_init = {0};
    AVDictionary* options = NULL;
    const AVInputFormat* infmt;
    FFIOContext sdp_pb;
    int ret = 0;
    av_log(p2p_ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) new track received (track_id=%d)\n", peer_connection_id, node->remote_id, tr);

    rtcDirection direct;
    rtcGetTrackDirection(tr, &direct);
    if (direct == RTC_DIRECTION_RECVONLY || direct == RTC_DIRECTION_SENDRECV) {
        // char ssrc[1024] = {};
        // ret = rtcGetSsrcsForTrack(tr, ssrc, 1024);//这个时候是没有ssrc的
        // av_log(ctx->avctx, AV_LOG_INFO, "ssrc is %s\n", ssrc);

        char sdp[1024] = {};
        ret = rtcGetTrackDescription(tr, sdp, sizeof(sdp));
        av_log(p2p_ctx->avctx, AV_LOG_INFO, "sdp is %s\n", sdp);

        char codec[1024] = {};
        char buffer[1024] = {};
        ret = rtcGetTrackPayloadTypesForCodec(tr,codec, buffer, sizeof(buffer));
        av_log(p2p_ctx->avctx, AV_LOG_INFO, "type is %s %s\n", codec, buffer);

        PeerConnectionTrack *track = av_malloc(sizeof(PeerConnectionTrack));
        track->track_id = tr;
        node->video_track = track;//debug临时试一下
        // track->avctx = p2p_ctx->avctx;
        // track->rtp_ctx = avformat_alloc_context();
        // if (!track->rtp_ctx) {
        //     av_log(p2p_ctx->avctx, AV_LOG_ERROR, "Failed to allocate RTP context\n");
        //     abort();
        //     return;
        // }
        // track->rtp_ctx->max_delay = track->avctx->max_delay;
        // track->rtp_ctx->interrupt_callback = track->avctx->interrupt_callback;
        //
        //
        // // 4. 初始化 SDP 文件的 IO 上下文
        // char sdp_track[SDP_MAX_SIZE] = { 0 };
        // ret = rtcGetTrackDescription(track->track_id, sdp_track, sizeof(sdp_track));
        // if (ret < 0) abort();
        // ffio_init_read_context(&sdp_pb, (uint8_t*)sdp_track, strlen(sdp_track));
        // track->rtp_ctx->pb = &sdp_pb.pub;
        //
        // // 5. 设置 SDP 选项, 打开 SDP 输入
        // av_dict_set(&options, "sdp_flags", "custom_io", 0);
        // infmt = av_find_input_format("sdp");
        // ret = avformat_open_input(&track->rtp_ctx, "temp.sdp", infmt, &options);
        // if (ret < 0) {
        //     av_log(p2p_ctx->avctx, AV_LOG_ERROR, "avformat_open_input failed\n");
        //     return;
        // }
        //
        // // 6. 打开文件描述符, 什么作用？
        // ret = ffio_fdopen(&track->rtp_ctx->pb, track->rtp_url_context);
        // if (ret < 0) {
        //     av_log(p2p_ctx->avctx, AV_LOG_ERROR, "ffio_fdopen failed\n");
        //     return;
        // }

        append_peer_connection_track_to_list(&node->track_caches, track);

    }
    p2p_ctx->waiting_for_sender = 0;
    //xy:TODO:optimize 处理 Track，例如：
    // - 绑定到播放器或录制模块
    // - 创建新的音视频解码器
}
