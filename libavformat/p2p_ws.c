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

    int ret = p2p_handle_signal_message(ctx, message, size);
    if (ret < 0) {
        printf("[FFmpegP2P][WebSocket] Failed to handle signal message: %d\n", ret);
    }
}
