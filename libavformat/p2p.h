//
// Created by Shaw on 2025/3/10.
//

#ifndef P2P_H
#define P2P_H

#include "rtc/rtc.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "cjson/cJSON.h"
#include <assert.h>

#include "avformat.h"
#include "url.h"

typedef enum P2PConnectionStatus {
    Disconnected,
    NetworkTesting,
    Connecting,
    Connected,
    Completed,
    Failed
} P2PConnectionStatus;

typedef struct NetworkQuality {

} NetworkQuality;

typedef struct PeerConnectionChannel {
    AVFormatContext *avctx;
    int track_id;
    AVFormatContext *rtp_ctx;
    URLContext *rtp_url_context;
    int channel_id;                     // Maybe track or data_channel.

    struct PeerConnectionChannel* next;
} PeerConnectionChannel;

typedef struct PeerConnectionNode {
    AVFormatContext* avctx;
    char *remote_id;
    int pc_id;                                  // peer_connection Wrap(function)
    // NetworkQuality NetworkQualityWhenInit;   //xy:Todo:如何动态更新P2P的网络状态？
    P2PConnectionStatus status;
    PeerConnectionChannel* data_channel_caches;
    PeerConnectionChannel* track_caches;
    struct PeerConnectionNode *next;
} PeerConnectionNode;

typedef struct P2PContext {
    AVFormatContext* avctx;
    rtcConfiguration* config;
    // 4位唯一id
    char* local_id;                     // local id应该根据本地外网IP和端口号生成，防止重复。

    // 信令服务器
    int web_socket;                     // signal_server_ws
    int web_socket_connected;
    char* web_socket_server_address;
    char* web_socket_server_port;
    // char* web_socket_local_id;
    // char* web_socket_remote_id;

    // peer
    PeerConnectionNode* peer_connection_caches;

} P2PContext;

void *p2p_main(void *arg);
int p2p_close_resource(P2PContext* const ctx);

int append_peer_connection_node_to_list(PeerConnectionNode **head, PeerConnectionNode *new_node);
int remove_peer_connection_node_from_list(PeerConnectionNode **head, const char *remote_id);
int release_peer_connection_node(PeerConnectionNode* node);
PeerConnectionNode* find_peer_connection_node_by_pc_id(PeerConnectionNode *head, int pc_id);
PeerConnectionNode* find_peer_connection_node_by_remote_id(PeerConnectionNode *head, const char *remote_id);

#endif //P2P_H
