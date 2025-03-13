/*
 * WebRTC-HTTP ingestion/egress protocol (WHIP/WHEP) common code
 *
 * Copyright (C) 2023 NativeWaves GmbH <contact@nativewaves.com>
 * This work is supported by FFG project 47168763.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVFORMAT_WEBRTC_H
#define AVFORMAT_WEBRTC_H

#include "avformat.h"
#include "avio_internal.h"
#include "libavcodec/codec_id.h"
#include "url.h"
#include "rtc/rtc.h"

#define RTP_MAX_PACKET_SIZE 1450

typedef struct DataChannelTrack {
    AVFormatContext *avctx;
    int track_id;
    AVFormatContext *rtp_ctx;
    URLContext *rtp_url_context;
} DataChannelTrack;

typedef struct DataChannelContext {
    AVFormatContext *avctx;
    int peer_connection;
    rtcState state;
    DataChannelTrack *tracks;
    int nb_tracks;
    const char *resource_location;

    /* options */
    char* bearer_token;
    int64_t connection_timeout;
    int64_t rw_timeout;

    pthread_t test_p2p_thread_id;
} DataChannelContext;



// 定义链表节点结构
typedef struct PeerConnectionNode {
    char *remote_id;
    int pc;                          // 可能是pc、也可能是dc、track， Wrap(function
    //Todo 后续补充的字段
    // typedef struct
    // {
    //     int connected = 0;
    //     rtcDirection direction = 0;
    // } TrackStatus;
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

    // track
    PeerConnectionNode* track_caches;

    pthread_t test_send_thread_id;
} P2PContext;

PeerConnectionNode * findPeerConnectionNodeByRemoteId(PeerConnectionNode *head, const char *remote_id);
PeerConnectionNode * findPeerConnectionNodeByPC(PeerConnectionNode *head, int pc);
void addPeerConnectionNodeToList(PeerConnectionNode **head, const char *remote_id, int pc);
//todo: void addDataChannelNodeToList(PeerConnectionNode **head, const char *remote_id, int pc);
void deletePeerConnection(PeerConnectionNode *head, const char *remote_id);

void on_ws_open_callback(int web_socket_id, void* ptr);
void on_ws_close_callback(int web_socket_id, void* ptr);
void on_ws_error_callback(int web_socket_id, const char *error, void *ptr);
void on_ws_message_callback(int web_socket_id, const char *message, int size, void *ptr);
void on_peer_connection_open_callback(int peer_connection_id, void* ptr);
void on_peer_connection_close_callback(int peer_connection_id, void* ptr);
void on_peer_connection_error_callback(int peer_connection_id, const char *error, void *ptr);
void on_peer_connection_message_callback(int peer_connection_id, const char *message, int size, void *ptr);
void on_pc_local_description_callback(int peer_connection_id, const char *sdp, const char *type, void *ptr);
void on_pc_local_candidate_callback(int peer_connection_id, const char *cand, const char *mid, void *ptr);
void on_pc_state_change_callback(int peer_connection_id, rtcState state, void *ptr);
void on_pc_ice_state_change_callback(int peer_connection_id, rtcIceState state, void *ptr);
void on_pc_gathering_state_callback(int peer_connection_id, rtcGatheringState state, void *ptr);
void on_pc_signaling_state_callback(int peer_connection_id, rtcSignalingState state, void *ptr);
void on_pc_data_channel_callback(int peer_connection_id, int dc, void *ptr);
void on_pc_track_callback(int peer_connection_id, int tr, void *ptr);
void on_data_channel_open_callback(int data_channel, void* ptr);
void on_data_channel_close_callback(int data_channel, void* ptr);
void on_data_channel_error_callback(int data_channel, const char *error, void *ptr);
void on_data_channel_message_callback(int data_channel, const char *message, int size, void *ptr);
void on_track_open_callback(int track_id, void* ptr);
void on_track_close_callback(int track_id, void* ptr);
void on_track_error_callback(int track_id, const char *error, void *ptr);
void on_track_message_callback(int track_id, const char *message, int size, void *ptr);

void *p2p_main(void *arg);
void* send_h264_main(void* ctx);
// int p2p_init_urlcontext();
int init_data_channel(P2PContext* const ctx, int peer_connection, char* remote_id);
int init_peer_connection(P2PContext* const ctx, char* remote_id);
int init_ws_resource(P2PContext* const ctx, char* web_socket_server_address, char* web_socket_server_port);
int p2p_close_resource(P2PContext* const ctx);

#define WEBRTC_OPTIONS(FLAGS, offset) \
    { "bearer_token", "optional Bearer token for authentication and authorization", offset+offsetof(DataChannelContext, bearer_token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS }, \
    { "connection_timeout", "timeout for establishing a connection", offset+offsetof(DataChannelContext, connection_timeout), AV_OPT_TYPE_DURATION, { .i64 = 10000000 }, 1, INT_MAX, FLAGS }, \
    { "rw_timeout", "timeout for receiving/writing data", offset+offsetof(DataChannelContext, rw_timeout), AV_OPT_TYPE_DURATION, { .i64 = 1000000 }, 1, INT_MAX, FLAGS }

extern int webrtc_close_resource(DataChannelContext*const ctx);
extern int webrtc_convert_codec(enum AVCodecID codec_id, rtcCodec* rtc_codec);
extern int webrtc_create_resource(DataChannelContext*const ctx);
extern void webrtc_deinit(DataChannelContext*const ctx);
extern int webrtc_generate_media_stream_id(char media_stream_id[37]);
extern int webrtc_init_connection(DataChannelContext*const ctx);
extern void webrtc_init_logger(void);
extern int webrtc_init_urlcontext(DataChannelContext*const ctx, int track_idx);

#endif /* AVFORMAT_WEBRTC_H */
