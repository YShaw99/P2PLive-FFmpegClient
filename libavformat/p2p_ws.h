//
// Created by Shaw on 2025/3/10.
//

#ifndef P2P_WS_H
#define P2P_WS_H

#include "p2p.h"
#include "p2p_signal.h"

void on_ws_open_callback(int web_socket_id, void* ptr);
void on_ws_close_callback(int web_socket_id, void* ptr);
void on_ws_error_callback(int web_socket_id, const char *error, void *ptr);
void on_ws_message_callback(int web_socket_id, const char *message, int size, void *ptr);

int init_ws_resource(P2PContext* const ctx, char* web_socket_server_address, char* web_socket_server_port);

#endif //P2P_WS_H
