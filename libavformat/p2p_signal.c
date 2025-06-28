//
// P2P Signal Protocol Implementation
// Created by Shaw on 2025/3/10.
//

#include "p2p_signal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <libavutil/uuid.h>
#include <libavutil/time.h>
#include <libavutil/random_seed.h>



// ==================== 信令消息处理 ====================

int p2p_send_signal_message(P2PContext* ctx, const P2PSignalMessage* msg) {
    if (!ctx || !msg) return AVERROR(EINVAL);
    
    cJSON* json = cJSON_CreateObject();
    if (!json) return AVERROR(ENOMEM);
    
    // 添加基本字段
    cJSON_AddStringToObject(json, "id", msg->id);
    cJSON_AddStringToObject(json, "type", p2p_message_type_to_string(msg->type));
    
    // 根据消息类型添加特定字段
    switch (msg->type) {
        case P2P_MSG_JOIN_ROOM:
            cJSON_AddStringToObject(json, "room_id", msg->payload.join_room.room_id);
            cJSON_AddStringToObject(json, "role", msg->payload.join_room.role);
            if (msg->payload.join_room.capabilities)
                cJSON_AddStringToObject(json, "capabilities", msg->payload.join_room.capabilities);
            break;
            
        case P2P_MSG_ROOM_JOINED:
            cJSON_AddStringToObject(json, "room_id", msg->payload.room_joined.room_id);
            cJSON_AddStringToObject(json, "role", msg->payload.room_joined.role);
            cJSON_AddNumberToObject(json, "max_receivers", msg->payload.room_joined.max_receivers);
            cJSON_AddNumberToObject(json, "current_receivers", msg->payload.room_joined.current_receivers);
            break;
            
        case P2P_MSG_OFFER:
            cJSON_AddStringToObject(json, "description", msg->payload.offer.description);
            break;
            
        case P2P_MSG_ANSWER:
            cJSON_AddStringToObject(json, "description", msg->payload.answer.description);
            break;
            
        case P2P_MSG_CANDIDATE:
            cJSON_AddStringToObject(json, "candidate", msg->payload.candidate.candidate);
            cJSON_AddStringToObject(json, "mid", msg->payload.candidate.mid);
            break;
            
        case P2P_MSG_PROBE_REQUEST:
            cJSON_AddStringToObject(json, "probe_id", msg->payload.probe_request.probe_id);
            cJSON_AddNumberToObject(json, "packet_size", msg->payload.probe_request.packet_size);
            break;
            
        case P2P_MSG_PROBE_RESPONSE:
            cJSON_AddStringToObject(json, "probe_id", msg->payload.probe_response.probe_id);
            cJSON_AddStringToObject(json, "status", msg->payload.probe_response.status);
            cJSON_AddNumberToObject(json, "timestamp", msg->payload.probe_response.timestamp);
            cJSON_AddNumberToObject(json, "crc", msg->payload.probe_response.crc);
            break;
            
        case P2P_MSG_ERROR:
            cJSON_AddStringToObject(json, "error_code", msg->payload.error.error_code);
            cJSON_AddStringToObject(json, "error_msg", msg->payload.error.error_msg);
            break;
            
        default:
            break;
    }
    
    char* json_str = cJSON_PrintUnformatted(json);
    int ret = rtcSendMessage(ctx->web_socket, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(json);
    
    return ret;
}

int p2p_handle_signal_message(P2PContext* ctx, const char* message, int size) {
    if (!ctx || !message) return AVERROR(EINVAL);
    
    cJSON* json = cJSON_Parse(message);
    if (!json) return AVERROR(EINVAL);
    
    // 解析基本字段
    cJSON* id_json = cJSON_GetObjectItem(json, "id");
    cJSON* type_json = cJSON_GetObjectItem(json, "type");
    
    if (!cJSON_IsString(id_json) || !cJSON_IsString(type_json)) {
        cJSON_Delete(json);
        return AVERROR(EINVAL);
    }
    
    P2PSignalMessage* msg = av_mallocz(sizeof(P2PSignalMessage));
    if (!msg) {
        cJSON_Delete(json);
        return AVERROR(ENOMEM);
    }
    
    msg->id = strdup(id_json->valuestring);
    msg->type = p2p_string_to_message_type(type_json->valuestring);
    
    // 根据消息类型解析特定字段
    switch (msg->type) {
        case P2P_MSG_JOIN_ROOM:
            {
                cJSON* room_id_json = cJSON_GetObjectItem(json, "room_id");
                cJSON* role_json = cJSON_GetObjectItem(json, "role");
                cJSON* capabilities_json = cJSON_GetObjectItem(json, "capabilities");
                
                if (cJSON_IsString(room_id_json)) {
                    msg->payload.join_room.room_id = strdup(room_id_json->valuestring);
                }
                if (cJSON_IsString(role_json)) {
                    msg->payload.join_room.role = strdup(role_json->valuestring);
                }
                if (cJSON_IsString(capabilities_json)) {
                    msg->payload.join_room.capabilities = strdup(capabilities_json->valuestring);
                }
            }
            break;
            
        case P2P_MSG_ROOM_JOINED:
            {
                cJSON* room_id_json = cJSON_GetObjectItem(json, "room_id");
                cJSON* role_json = cJSON_GetObjectItem(json, "role");
                cJSON* max_receivers_json = cJSON_GetObjectItem(json, "max_receivers");
                cJSON* current_receivers_json = cJSON_GetObjectItem(json, "current_receivers");
                
                if (cJSON_IsString(room_id_json)) {
                    msg->payload.room_joined.room_id = strdup(room_id_json->valuestring);
                }
                if (cJSON_IsString(role_json)) {
                    msg->payload.room_joined.role = strdup(role_json->valuestring);
                }
                if (cJSON_IsNumber(max_receivers_json)) {
                    msg->payload.room_joined.max_receivers = max_receivers_json->valueint;
                }
                if (cJSON_IsNumber(current_receivers_json)) {
                    msg->payload.room_joined.current_receivers = current_receivers_json->valueint;
                }
            }
            break;
            
        case P2P_MSG_OFFER:
            {
                cJSON* description_json = cJSON_GetObjectItem(json, "description");
                if (cJSON_IsString(description_json)) {
                    msg->payload.offer.description = strdup(description_json->valuestring);
                }
            }
            break;
            
        case P2P_MSG_ANSWER:
            {
                cJSON* description_json = cJSON_GetObjectItem(json, "description");
                if (cJSON_IsString(description_json)) {
                    msg->payload.answer.description = strdup(description_json->valuestring);
                }
            }
            break;
            
        case P2P_MSG_CANDIDATE:
            {
                cJSON* candidate_json = cJSON_GetObjectItem(json, "candidate");
                cJSON* mid_json = cJSON_GetObjectItem(json, "mid");
                if (cJSON_IsString(candidate_json)) {
                    msg->payload.candidate.candidate = strdup(candidate_json->valuestring);
                }
                if (cJSON_IsString(mid_json)) {
                    msg->payload.candidate.mid = strdup(mid_json->valuestring);
                }
            }
            break;
            
        case P2P_MSG_PROBE_REQUEST:
            {
                cJSON* probe_id_json = cJSON_GetObjectItem(json, "probe_id");
                cJSON* packet_size_json = cJSON_GetObjectItem(json, "packet_size");
                
                if (cJSON_IsString(probe_id_json)) {
                    msg->payload.probe_request.probe_id = strdup(probe_id_json->valuestring);
                }
                if (cJSON_IsNumber(packet_size_json)) {
                    msg->payload.probe_request.packet_size = packet_size_json->valueint;
                }
            }
            break;
            
        case P2P_MSG_PROBE_RESPONSE:
            {
                cJSON* probe_id_json = cJSON_GetObjectItem(json, "probe_id");
                cJSON* status_json = cJSON_GetObjectItem(json, "status");
                cJSON* timestamp_json = cJSON_GetObjectItem(json, "timestamp");
                cJSON* crc_json = cJSON_GetObjectItem(json, "crc");
                
                if (cJSON_IsString(probe_id_json)) {
                    msg->payload.probe_response.probe_id = strdup(probe_id_json->valuestring);
                }
                if (cJSON_IsString(status_json)) {
                    msg->payload.probe_response.status = strdup(status_json->valuestring);
                }
                if (cJSON_IsNumber(timestamp_json)) {
                    msg->payload.probe_response.timestamp = timestamp_json->valueint;
                }
                if (cJSON_IsNumber(crc_json)) {
                    msg->payload.probe_response.crc = (uint32_t)crc_json->valueint;
                }
            }
            break;
            
        case P2P_MSG_STREAM_REQUEST:
            {
                cJSON* stream_id_json = cJSON_GetObjectItem(json, "stream_id");
                if (cJSON_IsString(stream_id_json)) {
                    msg->payload.stream_request.stream_id = strdup(stream_id_json->valuestring);
                }
            }
            break;
            
        case P2P_MSG_STREAM_READY:
            {
                cJSON* stream_id_json = cJSON_GetObjectItem(json, "stream_id");
                if (cJSON_IsString(stream_id_json)) {
                    msg->payload.stream_ready.stream_id = strdup(stream_id_json->valuestring);
                }
            }
            break;
            
        default:
            break;
    }
    
    cJSON_Delete(json);
    
    // 调用回调函数处理消息
    int ret = 0;
    if (ctx->signal_callbacks && ctx->signal_callbacks->message_handler) {
        ret = ctx->signal_callbacks->message_handler(ctx, msg);
    }
    
    p2p_free_signal_message(msg);
    return ret;
}

void p2p_free_signal_message(P2PSignalMessage* msg) {
    if (!msg) {
        return;
    }

    if (msg->id) {
        free(msg->id);
        msg->id = NULL;
    }

    // 根据消息类型释放对应的payload内存
    switch (msg->type) {
        case P2P_MSG_OFFER:
            if (msg->payload.offer.description) {
                free(msg->payload.offer.description);
            }
            break;
        case P2P_MSG_ANSWER:
            if (msg->payload.answer.description) {
                free(msg->payload.answer.description);
            }
            break;
        case P2P_MSG_CANDIDATE:
            if (msg->payload.candidate.candidate) {
                free(msg->payload.candidate.candidate);
            }
            if (msg->payload.candidate.mid) {
                free(msg->payload.candidate.mid);
            }
            break;
        case P2P_MSG_PROBE_REQUEST:
            if (msg->payload.probe_request.probe_id) {
                free(msg->payload.probe_request.probe_id);
            }
            break;
        case P2P_MSG_PROBE_RESPONSE:
            if (msg->payload.probe_response.probe_id) {
                free(msg->payload.probe_response.probe_id);
            }
            if (msg->payload.probe_response.status) {
                free(msg->payload.probe_response.status);
            }
            break;
        case P2P_MSG_ROOM_JOINED:
            if (msg->payload.room_joined.room_id) {
                free(msg->payload.room_joined.room_id);
            }
            if (msg->payload.room_joined.role) {
                free(msg->payload.room_joined.role);
            }
            break;
        case P2P_MSG_STREAM_REQUEST:
            if (msg->payload.stream_request.stream_id) {
                free(msg->payload.stream_request.stream_id);
            }
            break;
        case P2P_MSG_STREAM_READY:
            if (msg->payload.stream_ready.stream_id) {
                free(msg->payload.stream_ready.stream_id);
            }
            break;
        default:
            break;
    }
    
    free(msg);
}


// ==================== 工具函数实现 ====================
P2PSignalMessage* p2p_create_signal_message(P2PMessageType type,
                                            const char* sender_id) {
    P2PSignalMessage* msg = (P2PSignalMessage*)malloc(sizeof(P2PSignalMessage));
    if (!msg) return NULL;
    memset(msg, 0, sizeof(P2PSignalMessage));

    msg->type = type;
    if (sender_id) {
        msg->id = strdup(sender_id);
    } else {
        msg->id = NULL;
    }
    // 其余payload字段由调用者后续赋值
    // 这里只做初始化，payload union内容全部置0
    memset(&msg->payload, 0, sizeof(msg->payload));

    return msg;
}

const char* p2p_message_type_to_string(P2PMessageType type) {
    switch (type) {
        case P2P_MSG_JOIN_ROOM:      return "join_room";
        case P2P_MSG_LEAVE_ROOM:     return "leave_room";
        case P2P_MSG_ROOM_JOINED:    return "room_joined";
        case P2P_MSG_ROOM_LEFT:      return "room_left";
        case P2P_MSG_OFFER:          return "offer";
        case P2P_MSG_ANSWER:         return "answer";
        case P2P_MSG_CANDIDATE:      return "candidate";
        case P2P_MSG_PROBE_REQUEST:  return "probe_request";
        case P2P_MSG_PROBE_RESPONSE: return "probe_response";
        case P2P_MSG_STREAM_REQUEST: return "stream_request";
        case P2P_MSG_STREAM_READY:   return "stream_ready";
        case P2P_MSG_ERROR:          return "error";
        default:                     return "unknown";
    }
}

P2PMessageType p2p_string_to_message_type(const char* type_str) {
    if (!type_str) return P2P_MSG_ERROR;
    
    if (strcmp(type_str, "join_room") == 0)      return P2P_MSG_JOIN_ROOM;
    if (strcmp(type_str, "leave_room") == 0)     return P2P_MSG_LEAVE_ROOM;
    if (strcmp(type_str, "room_joined") == 0)    return P2P_MSG_ROOM_JOINED;
    if (strcmp(type_str, "room_left") == 0)      return P2P_MSG_ROOM_LEFT;
    if (strcmp(type_str, "offer") == 0)          return P2P_MSG_OFFER;
    if (strcmp(type_str, "answer") == 0)         return P2P_MSG_ANSWER;
    if (strcmp(type_str, "candidate") == 0)      return P2P_MSG_CANDIDATE;
    if (strcmp(type_str, "probe_request") == 0)  return P2P_MSG_PROBE_REQUEST;
    if (strcmp(type_str, "probe_response") == 0) return P2P_MSG_PROBE_RESPONSE;
    if (strcmp(type_str, "stream_request") == 0) return P2P_MSG_STREAM_REQUEST;
    if (strcmp(type_str, "stream_ready") == 0)   return P2P_MSG_STREAM_READY;
    if (strcmp(type_str, "error") == 0)          return P2P_MSG_ERROR;
    
    return P2P_MSG_ERROR;
}

// ==================== 回调管理函数 ====================

int p2p_set_signal_message_handler(P2PContext* ctx, 
                                   int (*handler)(P2PContext*, const P2PSignalMessage*),
                                   void* user_data) {
    if (!ctx) return AVERROR(EINVAL);
    
    if (!ctx->callbacks) {
        ctx->callbacks = av_mallocz(sizeof(P2PSignalCallbacks));
        if (!ctx->callbacks) {
            return AVERROR(ENOMEM);
        }
    }
    
    ctx->callbacks->message_handler = handler;
    ctx->callbacks->user_data = user_data;
    
    return 0;
}
