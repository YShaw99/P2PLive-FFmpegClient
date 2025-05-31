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

typedef struct PeerConnectionNode {
    char *remote_id;
    int pc;                          // Maybe pc or dc， Wrap(function)
    struct PeerConnectionNode *next;
} PeerConnectionNode;

typedef struct P2PContext {
    rtcConfiguration* config;
    // 4位唯一id
    char* local_id;

    // 信令服务器
    int web_socket;
    char* web_socket_server_address;
    char* web_socket_server_port;
    char* web_socket_local_id;
    char* web_socket_remote_id;

    // pc
    PeerConnectionNode* peer_connection_caches;

    // dc
    PeerConnectionNode* data_channel_caches;

} P2PContext;

void *p2p_main(void *arg);
int p2p_close_resource(P2PContext* const ctx);

PeerConnectionNode * find_peer_connection_node_by_remote_id(PeerConnectionNode *head, const char *remote_id);
PeerConnectionNode * find_peer_connection_node_by_pc(PeerConnectionNode *head, int pc);
void add_peer_connection_node_to_list(PeerConnectionNode **head, const char *remote_id, int pc);
//todo: void addDataChannelNodeToList(PeerConnectionNode **head, const char *remote_id, int pc);
void delete_peer_connection_node_in_list(PeerConnectionNode *head, const char *remote_id);


#endif //P2P_H
