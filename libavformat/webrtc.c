/*
 * WebRTC-HTTP ingestion/egress protocol (WHIP/WHEP) common code
 *
 * Copyright (C) 2023 NativeWaves GmbH <contact@nativewaves.com>
 * This work is supported by FFG project 47168763.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/uuid.h"
#include "libavutil/random_seed.h"
#include "rtpenc_chain.h"
#include "rtsp.h"
#include "webrtc.h"

#include <pthread.h>

#include "cjson/cJSON.h"

// 1. 函数：webrtc_get_state_name
// 功能：根据 WebRTC 状态返回对应的状态名称字符串
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

// 2. 函数：webrtc_log
// 功能：将 WebRTC 日志转换为 FFmpeg 日志级别并输出
static void webrtc_log(const rtcLogLevel rtcLevel, const char *const message)
{
    int level = AV_LOG_VERBOSE;
    switch (rtcLevel)
    {
        case RTC_LOG_NONE:
            level = AV_LOG_QUIET;
            break;
        case RTC_LOG_DEBUG:
        case RTC_LOG_VERBOSE:
            level = AV_LOG_DEBUG;
            break;
        case RTC_LOG_INFO:
            level = AV_LOG_VERBOSE;
            break;
        case RTC_LOG_WARNING:
            level = AV_LOG_WARNING;
            break;
        case RTC_LOG_ERROR:
            level = AV_LOG_ERROR;
            break;
        case RTC_LOG_FATAL:
            level = AV_LOG_FATAL;
            break;
    }

    av_log(NULL, level, "[libdatachannel] %s\n", message);
}

// 3. 函数：webrtc_init_logger
// 功能：初始化 WebRTC 日志记录器，将 FFmpeg 日志级别映射到 WebRTC 日志级别
void webrtc_init_logger(void)
{
    rtcLogLevel level = RTC_LOG_VERBOSE;
    switch (av_log_get_level())
    {
        case AV_LOG_QUIET:
            level = RTC_LOG_NONE;
            break;
        case AV_LOG_DEBUG:
            level = RTC_LOG_DEBUG;
            break;
        case AV_LOG_VERBOSE:
            level = RTC_LOG_VERBOSE;
            break;
        case AV_LOG_WARNING:
            level = RTC_LOG_WARNING;
            break;
        case AV_LOG_ERROR:
            level = RTC_LOG_ERROR;
            break;
        case AV_LOG_FATAL:
            level = RTC_LOG_FATAL;
            break;
    }

    rtcInitLogger(level, webrtc_log);
}

// 4. 函数：webrtc_generate_media_stream_id
// 功能：生成唯一的媒体流 ID（UUID 格式）
int webrtc_generate_media_stream_id(char media_stream_id[37])
{
    int ret;
    AVUUID uuid;

    ret = av_random_bytes(uuid, sizeof(uuid));
    if (ret < 0) {
        goto fail;
    }
    av_uuid_unparse(uuid, media_stream_id);
    return 0;

fail:
    return ret;
}

// 5. 函数：webrtc_create_resource
// 功能：创建 WebRTC 资源，发送 SDP 提议并处理服务器响应
int webrtc_create_resource(DataChannelContext*const ctx)
{

    pthread_create(&ctx->test_p2p_thread_id, NULL, p2p_main, NULL);
    // pthread_detach(ctx->test_p2p_thread_id);

    sleep(1000);

    int ret;
    URLContext* h = NULL;
    char* headers;
    char offer_sdp[SDP_MAX_SIZE] = { 0 };
    char response_sdp[SDP_MAX_SIZE] = { 0 };

    /* 1.1 设置本地描述 */
    if (rtcSetLocalDescription(ctx->peer_connection, "offer") != RTC_ERR_SUCCESS) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to set local description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* 1.2 获取本地 SDP 提议 */
    ret = rtcGetLocalDescription(ctx->peer_connection, offer_sdp, sizeof(offer_sdp));
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to get local description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(ctx->avctx, AV_LOG_VERBOSE, "offer_sdp: %s\n", offer_sdp);

    /* 1.3 分配 HTTP 上下文 */
    if ((ret = ffurl_alloc(&h, ctx->avctx->url, AVIO_FLAG_READ_WRITE, NULL)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_alloc failed\n");
        goto fail;
    }

    /* 1.4 设置 HTTP 请求头 */
    headers = av_asprintf("Content-type: application/sdp\r\n");
    if (ctx->bearer_token) {
        headers = av_asprintf("%sAuthorization: Bearer %s\r\n", headers, ctx->bearer_token);
    }
    av_log(ctx->avctx, AV_LOG_VERBOSE, "headers: %s\n", headers);
    av_opt_set(h->priv_data, "headers", headers, 0);
    av_opt_set(h->priv_data, "method", "POST", 0);
    av_opt_set_bin(h->priv_data, "post_data", (uint8_t*)offer_sdp, strlen(offer_sdp), 0);

    /* 1.5 打开 HTTP 连接 */
    if ((ret = ffurl_connect(h, NULL)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_connect failed\n");
        goto fail;
    }

    /* 1.6 读取服务器响应 */
    ret = ffurl_read_complete(h, (unsigned char*)response_sdp, sizeof(response_sdp));
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_read_complete failed\n");
        goto fail;
    }

    av_log(ctx->avctx, AV_LOG_VERBOSE, "response: %s\n", response_sdp);

    /* 1.7 设置远程描述 */
    ret = rtcSetRemoteDescription(ctx->peer_connection, response_sdp, "answer");
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to set remote description\n");
        goto fail;
    }

    /* 1.8 保存资源位置 */
    av_opt_get(h->priv_data, "new_location", AV_OPT_SEARCH_CHILDREN, (uint8_t**)&ctx->resource_location);
    av_log(ctx->avctx, AV_LOG_VERBOSE, "resource_location: %s\n", ctx->resource_location);

    /* 1.9 关闭 HTTP 连接 */
    if ((ret = ffurl_closep(&h)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_closep failed\n");
        goto fail;
    }

    av_freep(&headers);
    return 0;

fail:
    if (h) {
        ffurl_closep(&h);
    }
    av_freep(&headers);
    return ret;
}


/**
 * @brief 根据 id 查找对应的 PeerConnection
 *
 * @param id 字符串 id
 * @return 如果找到返回对应的 pc，否则返回 -1
 */
PeerConnectionNode * findPeerConnectionNodeByRemoteId(PeerConnectionNode *head, const char *remote_id) {
    PeerConnectionNode *current = head;
    while (current != NULL) {
        if (strcmp(current->remote_id, remote_id) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

PeerConnectionNode * findPeerConnectionNodeByPC(PeerConnectionNode *head, int pc) {
    PeerConnectionNode *current = head;
    while (current != NULL) {
        if (current->pc == pc) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/**
 * @brief 向链表中添加新的映射项
 *
 * @param id 字符串 id
 * @param pc 对应的 PeerConnection（这里为 int）
 */
void addPeerConnectionNodeToList(PeerConnectionNode **head, const char *remote_id, int pc) {
    // 检查是否已存在
    if (findPeerConnectionNodeByRemoteId(*head, remote_id) != NULL) {
        printf("id %s 已存在，更新映射\n", remote_id);
        // 如果需要更新已有项，可在这里遍历找到对应节点并更新 pc
        PeerConnectionNode *current = *head;
        while (current != NULL) {
            if (strcmp(current->remote_id, remote_id) == 0) {
                current->pc = pc;
                return;
            }
            current = current->next;
        }
    }

    // 创建新的节点
    PeerConnectionNode *newNode = malloc(sizeof(PeerConnectionNode));
    if (!newNode) {
        fprintf(stderr, "内存分配失败\n");
        exit(EXIT_FAILURE);
    }
    newNode->remote_id = strdup(remote_id);  // 复制 id 字符串
    newNode->pc = pc;
    newNode->next = *head;      // 新节点插入到链表头
    *head = newNode;
}
/**
 * @brief 删除链表中某个 id 对应的映射项
 *
 * @param id 字符串 id
 */
void deletePeerConnection(PeerConnectionNode *head, const char *remote_id) {
    PeerConnectionNode *current = head, *prev = NULL;
    while (current != NULL) {
        if (strcmp(current->remote_id, remote_id) == 0) {
            if (prev == NULL) {  // 删除头节点
                head = current->next;
            } else {
                prev->next = current->next;
            }
            free(current->remote_id);
            free(current);
            printf("删除 id: %s 的映射\n", remote_id);
            return;
        }
        prev = current;
        current = current->next;
    }
    printf("未找到 id: %s\n", remote_id);
}

// -------- ws callback --------
void on_ws_open_callback(int web_socket_id, void* ptr)
{
    printf("[FFmpegP2P][WebSocket] opened | web_socket_id: %d\n", web_socket_id);
}

void on_ws_close_callback(int web_socket_id, void* ptr)
{
    printf("[FFmpegP2P][WebSocket] close | web_socket_id: %d\n", web_socket_id);
}

void on_ws_error_callback(int web_socket_id, const char *error, void *ptr)
{
    printf("[FFmpegP2P][WebSocket] error | error: %s, web_socket_id: %d\n", error, web_socket_id);
    assert(false);
}

void on_ws_message_callback(int web_socket_id, const char *message, int size, void *ptr)
{
    printf("[FFmpegP2P][WebSocket] message! web_socket_id: %d, message: %s \n", web_socket_id, message);

    P2PContext* ctx = (P2PContext*)ptr;
    cJSON* message_json = cJSON_Parse(message);
    if (message_json == NULL) {
        printf("[FFmpegP2P][WebSocket] message JSON parse error! Raw message: %s\n", message);
        return;
    }

    cJSON* remote_id_json = cJSON_GetObjectItem(message_json, "id");
    if (!cJSON_IsString(remote_id_json) || remote_id_json->valuestring == NULL)
    {
        printf("[FFmpegP2P][WebSocket] Invalid 'id' JSON format!\n");
        cJSON_Delete(message_json);
        return;
    }
    char* remote_id = remote_id_json->valuestring;

    cJSON* type_json = cJSON_GetObjectItem(message_json, "type");
    if(!cJSON_IsString(type_json) || type_json->valuestring == NULL) {
        printf("[FFmpegP2P][WebSocket] Invalid 'type' JSON format!\n");
        cJSON_Delete(message_json);
        return;
    }
    char* type = type_json->valuestring;


    // 打印解析后的 JSON
    char* json_string = cJSON_Print(message_json);
    printf("[FFmpegP2P][WebSocket] on_ws_message_callback | msg_json: %s, msg: %s\n",
           json_string ? json_string : "null", message);
    free(json_string);  // 释放 cJSON_Print 生成的字符串

    // 查找或创建 PeerConnection
    PeerConnectionNode* node = findPeerConnectionNodeByRemoteId(ctx->peer_connection_caches, remote_id);
    if (node != NULL && strcmp(type, "offer") == 0) {
        // 收到 offer 表示对方主动连接，新建 PeerConnection
        int ret = init_peer_connection(ctx->config, remote_id);
        if(ret != 0)
        {
            printf("[FFmpegP2P][WebSocket] init_peer_connection error!\n");
            goto end;
        }
        node = findPeerConnectionNodeByRemoteId(ctx->peer_connection_caches, remote_id);
    } else if (node == NULL) {
        // 如果没有连接，且类型不是 offer，则直接返回
        cJSON_Delete(message_json);
        goto end;
    }

    if (strcmp(type, "offer") == 0 || strcmp(type, "answer") == 0)
    {
        //对方主动发来连接请求
        cJSON* desc_json = cJSON_GetObjectItemCaseSensitive(message_json, "description");
        if (cJSON_IsString(desc_json) && desc_json->valuestring != NULL)
        {
            rtcSetRemoteDescription(node->pc, desc_json->valuestring, type);
        } else {
            printf("[FFmpegP2P][WebSocket] cJSON_GetObjectItemCaseSensitive(message_json, \"description\"); error!\n");
        }
    } else if (strcmp(type, "candidate") == 0)
    {
        cJSON* candidate_json = cJSON_GetObjectItemCaseSensitive(message_json, "candidate");
        cJSON* mid_json = cJSON_GetObjectItemCaseSensitive(message_json, "mid");
        if ((cJSON_IsString(candidate_json) && candidate_json->valuestring != NULL)
            && (cJSON_IsString(mid_json) && mid_json->valuestring != NULL))
        {
            rtcAddRemoteCandidate(node->pc, candidate_json->valuestring, mid_json->valuestring);
        } else
        {
            printf("[FFmpegP2P][WebSocket] cJSON_GetObjectItemCaseSensitive(message_json, \"candidate\"); error!\n");
        }
    }

    end:
    cJSON_Delete(message_json);
}


// -------- peer connect callback(common) --------
void on_peer_connection_open_callback(int peer_connection_id, void* ptr)
{
    printf("[FFmpegP2P][PeerConnection] connected | peer_connection_id: %d\n", peer_connection_id);
}

void on_peer_connection_close_callback(int peer_connection_id, void* ptr)
{
    printf("[FFmpegP2P][PeerConnection] close | peer_connection_id: %d\n", peer_connection_id);
}

void on_peer_connection_error_callback(int peer_connection_id, const char *error, void *ptr)
{
    printf("[FFmpegP2P][PeerConnection] error | error: %s, peer_connection_id: %d\n", error, peer_connection_id);
}

void on_peer_connection_message_callback(int peer_connection_id, const char *message, int size, void *ptr)
{
    printf("[FFmpegP2P][PeerConnection] message! peer_connection_id: %d, message: %s \n", peer_connection_id, message);
}

// -------- peer connect callback(pc only) --------
//这代表我方主动建连
void on_pc_local_description_callback(int peer_connection_id, const char *sdp, const char *type, void *ptr) {
    P2PContext* ctx = ptr;
    PeerConnectionNode* node = findPeerConnectionNodeByPC(ctx->peer_connection_caches, peer_connection_id);

    printf("[FFmpegP2P][PeerConnection] local_description | peer_connection_id: %d, sdp: %s, type: %s \n",
        peer_connection_id,
        sdp,
        type);

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
    rtcSendMessage(ctx->web_socket, json_str, size);

    // 释放内存
    free(json_str);
    cJSON_Delete(message);
}

void on_pc_local_candidate_callback(int peer_connection_id, const char *cand, const char *mid, void *ptr) {
    printf("[FFmpegP2P][PeerConnection] local_candidate\n");

    P2PContext* ctx = ptr;
    PeerConnectionNode* node = findPeerConnectionNodeByPC(ctx->peer_connection_caches, peer_connection_id);

    printf("[FFmpegP2P][PeerConnection] local_candidate | peer_connection_id: %d, cand: %s, mid: %s \n",
        peer_connection_id,
        cand,
        mid);

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

    free(json_str);
    cJSON_Delete(message);
}

void on_pc_state_change_callback(int peer_connection_id, rtcState state, void *ptr) {
    printf("[FFmpegP2P][PeerConnection] pc state change to %d \n", state);
}
void on_pc_ice_state_change_callback(int peer_connection_id, rtcIceState state, void *ptr) {
    printf("[FFmpegP2P][PeerConnection] on_pc_ice_state_change_callback | %d \n", state);
}
void on_pc_gathering_state_callback(int peer_connection_id, rtcGatheringState state, void *ptr) {
    printf("[FFmpegP2P][PeerConnection] on_pc_gathering_state_callback | %d \n", state);
}
void on_pc_signaling_state_callback(int peer_connection_id, rtcSignalingState state, void *ptr) {
    printf("[FFmpegP2P][PeerConnection] on_pc_signaling_state_callback | %d \n", state);
}
void on_pc_data_channel_callback(int peer_connection_id, int dc, void *ptr) {
    printf("[FFmpegP2P][PeerConnection] data_channel_callback | \n");
    P2PContext* ctx = ptr;

    //无论是主动还是被动，前面应该已经调用过init_data_channel了，这里只做一下查询，如果没有再调用一遍
    if (findPeerConnectionNodeByPC(ctx->data_channel_caches, dc) == NULL) {
        printf("data_channel_callback | [warning]⚠️ pc in dc caches is not found！ create new！！！\n");
        PeerConnectionNode *node = findPeerConnectionNodeByPC(ctx->peer_connection_caches, peer_connection_id);
        if(node == NULL)
        {
            assert(false);
            return;
        }
        init_data_channel(ctx, peer_connection_id, node->remote_id);
    } else
    {
        printf("data_channel_callback | pc in dc caches is found！ reuse\n");
    }

}
void on_pc_track_callback(int peer_connection_id, int tr, void *ptr) {
    printf("[FFmpegP2P][PeerConnection] ");
}

// -------- dc callback --------
void on_data_channel_open_callback(int data_channel, void* ptr)
{
    printf("[FFmpegP2P][DataChannel] connected | data_channel: %d\n", data_channel);
    P2PContext* ctx = ptr;
    PeerConnectionNode* node = findPeerConnectionNodeByPC(ctx->data_channel_caches, data_channel);
    //Todo: 这里没有做datachannel有效性检测，并且size=-1是以字符串发送，而非binary
    rtcSendMessage(data_channel, "Hello from 1", -1);
}

void on_data_channel_close_callback(int data_channel, void* ptr)
{
    printf("[FFmpegP2P][DataChannel] close | data_channel: %d\n", data_channel);
}

void on_data_channel_error_callback(int data_channel, const char *error, void *ptr)
{
    printf("[FFmpegP2P][DataChannel] error | error: %s, data_channel: %d\n", error, data_channel);
    //Todo: 这里可以做一些事
}

void on_data_channel_message_callback(int data_channel, const char *message, int size, void *ptr)
{
    printf("[FFmpegP2P][DataChannel] message! data_channel: %d, message: %s \n", data_channel, message);
}

// ----
int init_ws_resource(P2PContext* const ctx, char* web_socket_server_address, char* web_socket_server_port)
{
    assert(ctx);

#define WS_URL_SIZE 256
    char ws_url[WS_URL_SIZE];
    snprintf(ws_url, WS_URL_SIZE, "ws://%s:%s/%s", web_socket_server_address, web_socket_server_port, ctx->local_id);

    int web_socket = rtcCreateWebSocket(ws_url);
    ctx->web_socket = web_socket;
    ctx->web_socket_server_address = web_socket_server_address;
    ctx->web_socket_server_port = web_socket_server_port;

    rtcSetUserPointer(web_socket, ctx);
    rtcSetOpenCallback(web_socket, on_ws_open_callback);
    rtcSetErrorCallback(web_socket, on_ws_error_callback);
    rtcSetClosedCallback(web_socket, on_ws_close_callback);
    rtcSetMessageCallback(web_socket, on_ws_message_callback);

    return web_socket;
}

int init_peer_connection(P2PContext* const ctx, char* remote_id)
{
    int peer_connection = rtcCreatePeerConnection(ctx->config);
    addPeerConnectionNodeToList(&ctx->peer_connection_caches, remote_id, peer_connection);

    rtcSetUserPointer(peer_connection, ctx);
    // common
    rtcSetOpenCallback(peer_connection, on_peer_connection_open_callback);
    rtcSetErrorCallback(peer_connection, on_peer_connection_error_callback);
    rtcSetClosedCallback(peer_connection, on_peer_connection_close_callback);
    rtcSetMessageCallback(peer_connection, on_peer_connection_message_callback);
    // only pc
    rtcSetLocalDescriptionCallback(peer_connection, on_pc_local_description_callback);
    rtcSetLocalCandidateCallback(peer_connection, on_pc_local_candidate_callback);
    rtcSetStateChangeCallback(peer_connection, on_pc_state_change_callback);
    rtcSetIceStateChangeCallback(peer_connection, on_pc_ice_state_change_callback);
    rtcSetGatheringStateChangeCallback(peer_connection, on_pc_gathering_state_callback);
    rtcSetSignalingStateChangeCallback(peer_connection, on_pc_signaling_state_callback);
    // pc with dc
    rtcSetDataChannelCallback(peer_connection, on_pc_data_channel_callback);


    init_data_channel(ctx, peer_connection, remote_id);
    return 0;
}

int init_data_channel(P2PContext* const ctx, int peer_connection, char* remote_id)
{
    int data_channel = rtcCreateDataChannel(peer_connection, remote_id);
    addPeerConnectionNodeToList(&ctx->data_channel_caches, remote_id, data_channel);

    // common
    rtcSetOpenCallback(data_channel, on_data_channel_open_callback);
    rtcSetErrorCallback(data_channel, on_data_channel_error_callback);
    rtcSetClosedCallback(data_channel, on_data_channel_close_callback);
    rtcSetMessageCallback(data_channel, on_data_channel_message_callback);


    return 0;
}

void *p2p_main(void *arg)
{
    P2PContext* ctx = malloc(sizeof(P2PContext));
    memset(ctx, 0, sizeof(P2PContext));

    rtcConfiguration* config = malloc(sizeof(rtcConfiguration));
    memset(config, 0, sizeof(config));
    config->iceServersCount = 1;
    config->iceServers = (char*[]){"stun:stun.l.google.com:19302"};
    ctx->config = config;
    ctx->local_id = "send";

    // 1. 创建WebSocket连接
    init_ws_resource(ctx, "120.53.223.132", "8000");

    int peer_conncect_id = rtcCreatePeerConnection(config);
    // p2p_map[peer_conncect_id].peer_conncect_id = peer_conncect_id; // Todo 记录这个有啥用

    //我方主动建联的测试
    sleep(2);
    char* remote_id = "recv";
    init_peer_connection(ctx, remote_id);
    // //Todo后续改为cond
    // bool exit = 0;
    // while(exit==false)
    // {
    //
    // }
    return NULL;
}

int p2p_close_resource(P2PContext* const ctx)
{
    // // 清理
    // for (int i = 0; i < g_peerCount; ++i) {
    //     rtcDeletePeerConnection(g_peers[i].pcId);
    // }
    // rtcDeleteWebSocket(web_socket_id);
    // rtcCleanup();
    return 0;
}

// 6. 函数：webrtc_close_resource
// 功能：关闭 WebRTC 资源，发送 DELETE 请求释放资源
int webrtc_close_resource(DataChannelContext*const ctx)
{
    int ret;
    URLContext* h = NULL;
    char* headers = NULL;

    if (!ctx->resource_location) {
        return 0;
    }

    /* 2.1 分配 HTTP 上下文 */
    if ((ret = ffurl_alloc(&h, ctx->resource_location, AVIO_FLAG_READ_WRITE, NULL)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_alloc failed\n");
        goto fail;
    }

    /* 2.2 设置 HTTP 请求头 */
    if (ctx->bearer_token) {
        headers = av_asprintf("Authorization: Bearer %s\r\n", ctx->bearer_token);
        av_log(ctx->avctx, AV_LOG_VERBOSE, "headers: %s\n", headers);
    }
    av_opt_set(h->priv_data, "method", "DELETE", 0);

    /* 2.3 打开 HTTP 连接 */
    if ((ret = ffurl_connect(h, NULL)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_connect failed\n");
        goto fail;
    }

    /* 2.4 关闭 HTTP 连接 */
    if ((ret = ffurl_closep(&h)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_close failed\n");
        goto fail;
    }

fail:
    if (h) {
        ffurl_closep(&h);
    }
    av_freep(&ctx->resource_location);
    av_freep(&headers);
    return ret;
}

// 7. 函数：webrtc_read
// 功能：从 WebRTC 数据通道读取数据
static int webrtc_read(URLContext *h, unsigned char *buf, int size)
{
    const DataChannelTrack*const ctx = (const DataChannelTrack*const)h->priv_data;
    int ret;

    ret = rtcReceiveMessage(ctx->track_id, (char*)buf, &size);
    if (ret == RTC_ERR_NOT_AVAIL) {
        return AVERROR(EAGAIN);
    }
    else if (ret == RTC_ERR_TOO_SMALL) {
        return AVERROR_BUFFER_TOO_SMALL;
    }
    else if (ret != RTC_ERR_SUCCESS) {
        av_log(ctx->avctx, AV_LOG_ERROR, "rtcReceiveMessage failed: %d\n", ret);
        return AVERROR_EOF;
    }
    return size;
}

// 8. 函数：webrtc_write
// 功能：向 WebRTC 数据通道写入数据
static int webrtc_write(URLContext *h, const unsigned char *buf, int size)
{
    const DataChannelTrack*const ctx = (const DataChannelTrack*const)h->priv_data;
    int ret;

    ret = rtcSendMessage(ctx->track_id, (const char*)buf, size);
    if (ret != RTC_ERR_SUCCESS) {
        av_log(ctx->avctx, AV_LOG_ERROR, "rtcSendMessage failed: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    return size;
}

// 9. WebRTC URL 协议定义
static const URLProtocol ff_webrtc_protocol = {
    .name            = "webrtc",
    .url_read        = webrtc_read,
    .url_write       = webrtc_write,
};

// 10. 函数：webrtc_init_urlcontext
// 功能：初始化 WebRTC URL 上下文
int webrtc_init_urlcontext(DataChannelContext*const ctx, int track_idx)
{
    DataChannelTrack*const track = &ctx->tracks[track_idx];

    track->rtp_url_context = av_mallocz(sizeof(URLContext));
    if (!track->rtp_url_context) {
        return AVERROR(ENOMEM);
    }

    track->rtp_url_context->prot = &ff_webrtc_protocol;
    track->rtp_url_context->priv_data = track;
    track->rtp_url_context->max_packet_size = RTP_MAX_PACKET_SIZE;
    track->rtp_url_context->flags = AVIO_FLAG_READ_WRITE;
    track->rtp_url_context->rw_timeout = ctx->rw_timeout;
    return 0;
}

// 11. 函数：webrtc_on_state_change
// 功能：WebRTC 连接状态变化回调函数
static void webrtc_on_state_change(int pc, rtcState state, void* ptr)
{
    DataChannelContext*const ctx = (DataChannelContext*const)ptr;

    av_log(ctx->avctx, AV_LOG_VERBOSE, "Connection state changed from %s to %s\n", webrtc_get_state_name(ctx->state), webrtc_get_state_name(state));
    ctx->state = state;
}

// 12. 函数：webrtc_init_connection
// 功能：初始化 WebRTC 连接
int webrtc_init_connection(DataChannelContext *const ctx)
{
    int ret;
    rtcConfiguration config = { 0 };

    if (!(ctx->peer_connection = rtcCreatePeerConnection(&config))) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create PeerConnection\n");
        return AVERROR_EXTERNAL;
    }

    rtcSetUserPointer(ctx->peer_connection, ctx);

    if (rtcSetStateChangeCallback(ctx->peer_connection, webrtc_on_state_change)) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to set state change callback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    return 0;

fail:
    rtcDeletePeerConnection(ctx->peer_connection);
    return ret;
}

// 13. 函数：webrtc_convert_codec
// 功能：将 FFmpeg 编解码器 ID 转换为 WebRTC 编解码器类型
int webrtc_convert_codec(enum AVCodecID codec_id, rtcCodec* rtc_codec)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            *rtc_codec = RTC_CODEC_H264;
            break;
        case AV_CODEC_ID_HEVC:
            *rtc_codec = RTC_CODEC_H265;
            break;
        case AV_CODEC_ID_AV1:
            *rtc_codec = RTC_CODEC_AV1;
            break;
        case AV_CODEC_ID_VP9:
            *rtc_codec = RTC_CODEC_VP9;
            break;
        case AV_CODEC_ID_OPUS:
            *rtc_codec = RTC_CODEC_OPUS;
            break;
        case AV_CODEC_ID_AAC:
            *rtc_codec = RTC_CODEC_AAC;
            break;
        case AV_CODEC_ID_PCM_ALAW:
            *rtc_codec = RTC_CODEC_PCMA;
            break;
        case AV_CODEC_ID_PCM_MULAW:
            *rtc_codec = RTC_CODEC_PCMU;
            break;
        default:
            *rtc_codec = -1;
            return AVERROR(EINVAL);
    }

    return 0;
}

// 14. 函数：webrtc_deinit
// 功能：释放 WebRTC 相关资源
void webrtc_deinit(DataChannelContext*const ctx)
{
    if (ctx->tracks) {
        for (int i = 0; i < ctx->nb_tracks; ++i) {
            if (ctx->tracks[i].rtp_ctx)
                avformat_free_context(ctx->tracks[i].rtp_ctx);
            if (ctx->tracks[i].rtp_url_context)
                av_freep(&ctx->tracks[i].rtp_url_context);
            if (ctx->tracks[i].track_id)
                rtcDeleteTrack(ctx->tracks[i].track_id);
        }
        av_freep(&ctx->tracks);
    }
    if (ctx->peer_connection) {
        rtcDeletePeerConnection(ctx->peer_connection);
        ctx->peer_connection = 0;
    }
    if (ctx->resource_location)
        av_freep(&ctx->resource_location);
}