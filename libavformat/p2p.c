//
// Created by Shaw on 2025/3/10.
//

#include "p2p.h"
#include "avio.h"
#include "url.h"

int p2p_close_resource(P2PContext* const ctx) {
    // // 清理
    // for (int i = 0; i < g_peerCount; ++i) {
    //     rtcDelete_Peer_Connection_Node_In_List(g_peers[i].pcId);
    // }
    // rtcDeleteWebSocket(web_socket_id);
    // rtcCleanup();
    return 0;
}


// ---------
static int p2p_rtp_read(URLContext *h, unsigned char *buf, int size) {
    const PeerConnectionTrack * const track = (const PeerConnectionTrack * const)h->priv_data;
    int ret;

    ret = rtcReceiveMessage(track->track_id, (char*)buf, &size);
    if (ret == RTC_ERR_NOT_AVAIL) {
        return AVERROR(EAGAIN);
    } else if (ret == RTC_ERR_TOO_SMALL) {
        return AVERROR_BUFFER_TOO_SMALL;
    } else if (ret != RTC_ERR_SUCCESS) {
        av_log(track->avctx, AV_LOG_ERROR, "rtcReceiveMessage failed: %d\n", ret);
        return AVERROR_EOF;
    }
    return size;
}

static int p2p_rtp_write(URLContext *h, const unsigned char *buf, int size) {
    const PeerConnectionTrack * const track = (const PeerConnectionTrack * const)h->priv_data;
    int ret;

    ret = rtcSendMessage(track->track_id, (const char*)buf, size);
    if (ret != RTC_ERR_SUCCESS) {
        av_log(track->avctx, AV_LOG_ERROR, "rtcSendMessage failed: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    return size;
}

static const URLProtocol ff_p2p_rtp_protocol = {
    .name            = "p2p_rtp",
    .url_read        = p2p_rtp_read,
    .url_write       = p2p_rtp_write,
};

int p2p_rtp_init_urlcontext(PeerConnectionTrack * const track) {
    track->rtp_url_context = av_mallocz(sizeof(URLContext));
    if (track->rtp_url_context == NULL) {
        return AVERROR(ENOMEM);
    }

    track->rtp_url_context->prot = &ff_p2p_rtp_protocol;
    track->rtp_url_context->priv_data = track;
    track->rtp_url_context->max_packet_size = P2P_RTP_MAX_PACKET_SIZE;
    track->rtp_url_context->flags = AVIO_FLAG_READ_WRITE;
    //xy:ToDo: options
    track->rtp_url_context->rw_timeout = INT_MAX;// ctx->rw_timeout;
    return 0;
}

// PeerConnectionNode Utils
PeerConnectionNode* find_peer_connection_node_by_remote_id(PeerConnectionNode *head, const char *remote_id) {
    if (remote_id == NULL)
        return NULL;

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

int release_peer_connection_node(PeerConnectionNode* node) {
    if (node->remote_id != NULL)
        free(node->remote_id);
    //xy:TODO:release: 关于连接关闭，有哪些要做的工作？
    //清空p2p连接
    //清空datachannels连接
    //清空tracks的连接
    //清空config下所有字符串内存
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
    if (new_node == NULL)
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

// PeerConnectionTrack Utils
PeerConnectionTrack* find_peer_connection_track_by_track_id(PeerConnectionTrack *head, int track_id) {
    while (head) {
        if (head->track_id == track_id)
            return head;
        head = head->next;
    }
    return NULL;
}

PeerConnectionTrack* find_peer_connection_track_by_stream_index(PeerConnectionTrack *head, int stream_index) {
    while (head) {
        if (head->stream_index == stream_index)
            return head;
        head = head->next;
    }
    return NULL;
}

int release_peer_connection_track(PeerConnectionTrack* node) {
    //xy:TODO:release: 关于连接关闭，有哪些要做的工作？
    //调用rtc关闭channels
    return 0;
}

int remove_peer_connection_track_from_list(PeerConnectionTrack **head, const int track_id) {
    if (head == NULL)
        return AVERROR_INVALIDDATA;

    PeerConnectionTrack *prev = NULL, *curr = *head;
    while (curr) {
        if (curr->track_id == track_id) {
            if (prev)
                prev->next = curr->next;
            else
                *head = curr->next;

            // 释放节点内存
            release_peer_connection_track(curr);
            free(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    return 0;
}

int append_peer_connection_track_to_list(PeerConnectionTrack **head, PeerConnectionTrack *new_track) {
    if (new_track == NULL)
        return AVERROR_INVALIDDATA;
    int ret = 0;

    if (find_peer_connection_track_by_track_id(*head, new_track->track_id)) {
        if (ret = remove_peer_connection_track_from_list(head, new_track->track_id))
            return ret;
    }

    if (*head == NULL) {
        *head = new_track;
    } else {
        PeerConnectionTrack *tail = *head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = new_track;
    }

    return 0;
}

int p2p_generate_media_stream_id(char media_stream_id[37]) {
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