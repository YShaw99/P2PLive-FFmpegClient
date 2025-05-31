//
// Created by Shaw on 2025/3/10.
//

#ifndef P2P_PC_H
#define P2P_PC_H

#include "p2p.h"

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

int init_peer_connection(P2PContext* const ctx, char* remote_id);

#endif //P2P_PC_H
