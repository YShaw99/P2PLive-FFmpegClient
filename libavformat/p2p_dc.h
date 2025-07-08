//
// Created by Shaw on 2025/3/10.
//

#ifndef P2P_DC_H
#define P2P_DC_H

#include "p2p.h"

// ==================== Track 回调函数声明 ====================
void on_track_open_callback(int track, void *ptr);
void on_track_close_callback(int track, void *ptr);
void on_track_error_callback(int track, const char *error, void *ptr);
void on_track_message_callback(int track, const char *message, int size, void *ptr);

// ==================== Track 创建和设置函数声明 ====================

/**
 * 设置通用 Track 结构与回调
 * @param node PeerConnectionNode 指针
 * @param track PeerConnectionTrack 指针
 * @param track_id track ID
 * @param track_type track 类型
 * @return 成功返回0，失败返回负数错误码
 */
int setup_track_common_logic(PeerConnectionNode* node,
                             PeerConnectionTrack* track,
                             int track_id,
                             int stream_index,
                             PeerConnectionTrackType track_type);
/**
 * 设置 Probe Track 专用结构与回调
 * @param node PeerConnectionNode 指针
 * @param track PeerConnectionTrack 指针
 * @param datachannel_id DataChannel ID
 * @return 成功返回0，失败返回负数错误码
 */
int setup_probe_track(PeerConnectionNode* node,
                             PeerConnectionTrack* track,
                             int datachannel_id);

// ==================== Track 初始化函数声明 ====================
int create_send_track(AVFormatContext* avctx,
                      AVStream* stream,
                      PeerConnectionNode* node,
                      PeerConnectionTrack* track,
                      int index);

int init_send_track_ex(AVFormatContext* avctx,
                       AVStream* stream,
                       PeerConnectionNode* const node,
                       PeerConnectionTrack* const track,
                       int track_id,
                       int index);

int create_recv_track(AVFormatContext* avctx,
                      AVStream* stream,
                      PeerConnectionNode* node,
                      PeerConnectionTrack* track,
                      int index,
                      int payload_type,
                      uint32_t ssrc);

int init_recv_track_ex(AVFormatContext* avctx,
                       AVStream* stream,
                       PeerConnectionNode* const node,
                       PeerConnectionTrack* const track,
                       PeerConnectionTrackType track_type,
                       int track_id
                       // ,
                       // int payload_type,
                       // uint32_t ssrc,
                       // int index
                       );

int init_probe_track_ex(AVFormatContext *avctx,
                        PeerConnectionNode *const node,
                        PeerConnectionTrack *const track);

#endif // P2P_DC_H
