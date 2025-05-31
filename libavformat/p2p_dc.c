//
// Created by Shaw on 2025/3/4.
//
#include "p2p_dc.h"

int init_data_channel(P2PContext* const ctx, int peer_connection, char* remote_id)
{
    int data_channel = rtcCreateDataChannel(peer_connection, remote_id);
    add_peer_connection_node_to_list(&ctx->data_channel_caches, remote_id, data_channel);

    // common
    rtcSetOpenCallback(data_channel, on_data_channel_open_callback);
    rtcSetErrorCallback(data_channel, on_data_channel_error_callback);
    rtcSetClosedCallback(data_channel, on_data_channel_close_callback);
    rtcSetMessageCallback(data_channel, on_data_channel_message_callback);

    return 0;
}

// -------- dc callback --------
void on_data_channel_open_callback(int data_channel, void* ptr)
{
    printf("[FFmpegP2P][DataChannel] connected | data_channel: %d\n", data_channel);
    P2PContext* ctx = ptr;
    PeerConnectionNode* node = find_peer_connection_node_by_pc(ctx->data_channel_caches, data_channel);
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
