//
// Created by Shaw on 2025/3/10.
//

#include "p2p.h"

PeerConnectionNode* find_peer_connection_node_by_remote_id(PeerConnectionNode *head, const char *remote_id) {
    while (head) {
        if (head->remote_id && strcmp(head->remote_id, remote_id) == 0)
            return head;
        head = head->next;
    }
    return NULL;
}

PeerConnectionNode* find_peer_connection_node_by_pc_id(PeerConnectionNode *head, int pc_id) {
    while (head) {
        if (head->pc_id == pc_id)
            return head;
        head = head->next;
    }
    return NULL;
}

int release_peer_connection_node(PeerConnectionNode* node)
{
    if (node->remote_id != NULL)
        free(node->remote_id);
    //xy:TODO:release: 关于连接关闭，有哪些要做的工作？
    //清空p2p连接
    //清空datachannels连接
    //清空tracks的连接
    return 0;
}

int remove_peer_connection_node_from_list(PeerConnectionNode **head, const char *remote_id) {
    if (head == NULL || remote_id == NULL)
        return AVERROR_INVALIDDATA;

    PeerConnectionNode *prev = NULL, *curr = *head;
    while (curr) {
        if (curr->remote_id && strcmp(curr->remote_id, remote_id) == 0) {
            if (prev)
                prev->next = curr->next;
            else
                *head = curr->next;

            // 释放节点内存
            release_peer_connection_node(curr);
            free(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    return 0;
}

int append_peer_connection_node_to_list(PeerConnectionNode **head, PeerConnectionNode *new_node) {
    if (!new_node)
        return AVERROR_INVALIDDATA;
    int ret = 0;

    if (find_peer_connection_node_by_remote_id(*head, new_node->remote_id)) {
        if (ret = remove_peer_connection_node_from_list(head, new_node->remote_id))
            return ret;
    }

    if (*head == NULL) {
        *head = new_node;
    } else {
        PeerConnectionNode *tail = *head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = new_node;
    }

    return 0;
}

//Todo：这里后面直接写入muxer，然后封装改造p2p_create_resource
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
    // init_ws_resource(ctx, "120.53.223.132", "8000");

    int peer_conncect_id = rtcCreatePeerConnection(config);
    // p2p_map[peer_conncect_id].peer_conncect_id = peer_conncect_id; // Todo 记录这个有啥用

    //我方主动建联的测试
    // sleep(2);
    char* remote_id = "recv";
    // init_peer_connection(ctx, remote_id);
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
    //     rtcDelete_Peer_Connection_Node_In_List(g_peers[i].pcId);
    // }
    // rtcDeleteWebSocket(web_socket_id);
    // rtcCleanup();
    return 0;
}
