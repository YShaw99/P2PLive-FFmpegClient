//
// Created by Shaw on 2025/3/4.
//
#include "p2p_ws.h"
#include "p2p_pc.h"
#define WS_URL_SIZE 256

int init_ws_resource(P2PContext* const ctx, char* web_socket_server_address, char* web_socket_server_port) {
	if(ctx == NULL || web_socket_server_address == NULL || web_socket_server_port == NULL) {
		return AVERROR_INVALIDDATA;
	}

    char ws_url[WS_URL_SIZE];
    snprintf(ws_url, WS_URL_SIZE, "ws://%s:%s/%s", web_socket_server_address, web_socket_server_port, ctx->local_id);

    int web_socket = rtcCreateWebSocket(ws_url);
    ctx->web_socket = web_socket;
    ctx->web_socket_server_address = web_socket_server_address;
    ctx->web_socket_server_port = web_socket_server_port;
    ctx->web_socket_connected = 0;

    rtcSetUserPointer(web_socket, ctx);
    rtcSetOpenCallback(web_socket, on_ws_open_callback);
    rtcSetErrorCallback(web_socket, on_ws_error_callback);
    rtcSetClosedCallback(web_socket, on_ws_close_callback);
    rtcSetMessageCallback(web_socket, on_ws_message_callback);

    return 0;
}

// -------- ws callback --------
void on_ws_open_callback(int web_socket_id, void* ptr) {
    if (ptr == NULL) {
        printf("[FFmpegP2P][WebSocket] opened | web_socket_id: %d\n", web_socket_id);
    }
    P2PContext* ctx = (P2PContext*)ptr;
    ctx->web_socket_connected = 1;
    char* local_id = ctx->local_id;
}

void on_ws_close_callback(int web_socket_id, void* ptr) {
    printf("[FFmpegP2P][WebSocket] close | web_socket_id: %d\n", web_socket_id);

    P2PContext* ctx = (P2PContext*)ptr;
    ctx->web_socket_connected = 0;
}

void on_ws_error_callback(int web_socket_id, const char *error, void *ptr) {
    printf("[FFmpegP2P][WebSocket] error | error: %s, web_socket_id: %d\n", error, web_socket_id);

    P2PContext* ctx = (P2PContext*)ptr;
    ctx->web_socket_connected = 0;
    assert(false);
}

void on_ws_message_callback(int web_socket_id, const char *message, int size, void *ptr) {
    printf("[FFmpegP2P][WebSocket] message! web_socket_id: %d, message: %s \n", web_socket_id, message);

    P2PContext* ctx = (P2PContext*)ptr;
    /*
        int ret;
    cJSON* message_json = cJSON_Parse(message);
    if (message_json == NULL) {
        printf("[FFmpegP2P][WebSocket] message JSON parse error! Raw message: %s\n", message);
        return;
    }

    cJSON* remote_id_json = cJSON_GetObjectItem(message_json, "id");
    if (!cJSON_IsString(remote_id_json) || remote_id_json->valuestring == NULL) {
        printf("[FFmpegP2P][WebSocket] Invalid 'id' JSON format!\n");
        cJSON_Delete(message_json);
        return;
    }
    char remote_id[1024] = {};
    strcpy(remote_id, remote_id_json->valuestring);

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
    free(json_string);

    // 查找或创建 PeerConnection
    PeerConnectionNode* node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
    if (node == NULL && strcmp(type, "offer") == 0) {
        //xy:Todo:对方offer的时候，不能重建且覆盖，否则会崩溃，暂时没有捋顺该逻辑。但目前注释之后跑通了。
        // 收到 offer 表示对方主动连接，新建 PeerConnection
        ret = rtcDeletePeerConnection(node->pc_id);
        if (ret != RTC_ERR_SUCCESS) {
            abort();
        }
        //track收不到回调的问题，这里通过更新pc重试！
        ret = init_peer_connection(ctx, remote_id);
        if(ret != 0)
        {
            printf("[FFmpegP2P][WebSocket] init_peer_connection error!\n");
            goto end;
        }
        node = find_peer_connection_node_by_remote_id(ctx->peer_connection_node_caches, remote_id);
        // ctx->waiting_for_sender = 0;
    } else if (node == NULL) {
        // 如果没有连接，且类型不是 offer，则直接返回
        goto end;
    }

    if (strcmp(type, "offer") == 0 || strcmp(type, "answer") == 0) {
        //对方主动发来连接请求
        cJSON* desc_json = cJSON_GetObjectItemCaseSensitive(message_json, "description");
        if (cJSON_IsString(desc_json) && desc_json->valuestring != NULL)
        {
            rtcSetRemoteDescription(node->pc_id, desc_json->valuestring, type);
        } else {
            printf("[FFmpegP2P][WebSocket] cJSON_GetObjectItemCaseSensitive(message_json, \"description\"); error!\n");
        }
    } else if (strcmp(type, "candidate") == 0) {
        cJSON* candidate_json = cJSON_GetObjectItemCaseSensitive(message_json, "candidate");
        cJSON* mid_json = cJSON_GetObjectItemCaseSensitive(message_json, "mid");
        if ((cJSON_IsString(candidate_json) && candidate_json->valuestring != NULL)
            && (cJSON_IsString(mid_json) && mid_json->valuestring != NULL))
        {
            rtcAddRemoteCandidate(node->pc_id, candidate_json->valuestring, mid_json->valuestring);
        } else
        {
            printf("[FFmpegP2P][WebSocket] cJSON_GetObjectItemCaseSensitive(message_json, \"candidate\"); error!\n");
        }
    }

end:
    cJSON_Delete(message_json);
    */
    // 使用新的信令消息处理函数
    int ret = p2p_handle_signal_message(ctx, message, size);
    if (ret < 0) {
        printf("[FFmpegP2P][WebSocket] Failed to handle signal message: %d\n", ret);
    }
}
