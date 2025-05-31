//
// Created by Shaw on 2025/3/4.
//
#include "p2p_pc.h"
#include "p2p_dc.h"

int init_peer_connection(P2PContext* const ctx, char* remote_id)
{
    int peer_connection = rtcCreatePeerConnection(ctx->config);
    add_peer_connection_node_to_list(&ctx->peer_connection_caches, remote_id, peer_connection);

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
    PeerConnectionNode* node = find_peer_connection_node_by_pc(ctx->peer_connection_caches, peer_connection_id);

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
    PeerConnectionNode* node = find_peer_connection_node_by_pc(ctx->peer_connection_caches, peer_connection_id);

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
    if (find_peer_connection_node_by_pc(ctx->data_channel_caches, dc) == NULL) {
        printf("data_channel_callback | [warning]⚠️ pc in dc caches is not found！ create new！！！\n");
        PeerConnectionNode *node = find_peer_connection_node_by_pc(ctx->peer_connection_caches, peer_connection_id);
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
