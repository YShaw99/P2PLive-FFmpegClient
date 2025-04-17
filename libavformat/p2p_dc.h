//
// Created by Shaw on 2025/3/10.
//

#ifndef P2P_DC_H
#define P2P_DC_H

#include "p2p.h"

void on_track_open_callback(int track, void* ptr);
void on_track_close_callback(int track, void* ptr);
void on_track_error_callback(int track, const char *error, void *ptr);
void on_track_message_callback(int track, const char *message, int size, void *ptr);

//deprecated
int init_track(PeerConnectionNode* const node, char* remote_id);
int init_track_ex(AVFormatContext* avctx,
                  AVStream* stream,
                  PeerConnectionNode* const node,
                  PeerConnectionTrack* const track,
                  int index);

#endif //P2P_DC_H
