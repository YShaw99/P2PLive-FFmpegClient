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
#include <libavutil/random_seed.h>
#include <libavutil/uuid.h>
#include "version.h"

#include "avformat.h"
#include "url.h"

typedef enum P2PConnectionStatus {
    Disconnected,
    NetworkTesting,
    Connecting,
    Connected,
    Selected,
    Completed,
    Failed
} P2PConnectionStatus;

typedef struct NetworkQuality {

} NetworkQuality;

// Track 级别
typedef struct PeerConnectionTrack {
    AVFormatContext *avctx;
    AVFormatContext *rtp_ctx;
    URLContext *rtp_url_context;
    int track_id;                               // Maybe track or data_channel.
    int stream_index;

    struct PeerConnectionTrack* next;
} PeerConnectionTrack;

// user 级别
typedef struct PeerConnectionNode {
    AVFormatContext* avctx;
    struct P2PContext* p2p_ctx;
    const char *remote_id;
    int pc_id;                                  // peer_connection Wrap(function)
    // NetworkQuality NetworkQualityWhenInit;   //xy:Todo:如何动态更新P2P的网络状态？
    P2PConnectionStatus status;
    PeerConnectionTrack* track_caches;
    struct PeerConnectionNode *next;
} PeerConnectionNode;

typedef struct P2PContext {
    AVFormatContext* avctx;
    rtcConfiguration* config;
    // 4位唯一id
    char* local_id;                             // local id应该根据本地外网IP和端口号生成，防止重复。

    // 信令服务器
    int web_socket;                             // signal_server_ws
    int web_socket_connected;
    char* web_socket_server_address;
    char* web_socket_server_port;
    // char* web_socket_local_id;
    // char* web_socket_remote_id;

    PeerConnectionNode* peer_connection_node_caches;
    PeerConnectionNode* selected_node;

} P2PContext;

void *p2p_main(void *arg);
int p2p_close_resource(P2PContext* const ctx);
int p2p_rtp_init_urlcontext(PeerConnectionNode*const node, PeerConnectionTrack * const track);

int append_peer_connection_node_to_list(PeerConnectionNode **head, PeerConnectionNode *new_node);
int remove_peer_connection_node_from_list(PeerConnectionNode **head, const char *remote_id);
int release_peer_connection_node(PeerConnectionNode* node);
PeerConnectionNode* find_peer_connection_node_by_pc_id(PeerConnectionNode *head, int pc_id);
PeerConnectionNode* find_peer_connection_node_by_remote_id(PeerConnectionNode *head, const char *remote_id);

int append_peer_connection_track_to_list(PeerConnectionTrack **head, PeerConnectionTrack *new_track);
int remove_peer_connection_track_from_list(PeerConnectionTrack **head, const int track_id);
int release_peer_connection_track(PeerConnectionTrack* node);
PeerConnectionTrack* find_peer_connection_track_by_track_id(PeerConnectionTrack *head, int track_id);
PeerConnectionTrack* find_peer_connection_track_by_stream_index(PeerConnectionTrack *head, int stream_index);

int p2p_generate_media_stream_id(char media_stream_id[37]);

#endif //P2P_H
