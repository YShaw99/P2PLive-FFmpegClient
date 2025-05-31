//
// Created by Shaw on 2025/3/10.
//

#include "p2p.h"

/**
 * @brief 根据 id 查找对应的 PeerConnection
 *
 * @param id 字符串 id
 * @return 如果找到返回对应的 pc，否则返回 -1
 */
PeerConnectionNode * find_peer_connection_node_by_remote_id(PeerConnectionNode *head, const char *remote_id) {
    PeerConnectionNode *current = head;
    while (current != NULL) {
        if (strcmp(current->remote_id, remote_id) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

PeerConnectionNode * find_peer_connection_node_by_pc(PeerConnectionNode *head, int pc) {
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
void add_peer_connection_node_to_list(PeerConnectionNode **head, const char *remote_id, int pc) {
    // 检查是否已存在
    if (find_peer_connection_node_by_remote_id(*head, remote_id) != NULL) {
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
void delete_peer_connection_node_in_list(PeerConnectionNode *head, const char *remote_id) {
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
