//
// Created by Shaw on 2025/3/10.
//

#ifndef P2P_DC_H
#define P2P_DC_H

#include "p2p.h"

void on_data_channel_open_callback(int data_channel, void* ptr);
void on_data_channel_close_callback(int data_channel, void* ptr);
void on_data_channel_error_callback(int data_channel, const char *error, void *ptr);
void on_data_channel_message_callback(int data_channel, const char *message, int size, void *ptr);

int init_data_channel(P2PContext* const ctx, int peer_connection, char* remote_id);

#endif //P2P_DC_H
