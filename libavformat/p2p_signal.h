//
// P2P Signal Protocol Header
// Created by Shaw on 2025/3/10.
//

#ifndef P2P_SIGNAL_H
#define P2P_SIGNAL_H

#include <stdint.h>
#include "p2p.h"
#include "cjson/cJSON.h"

// ==================== 信令消息结构 ====================
typedef enum P2PMessageType {
    P2P_MSG_JOIN_ROOM,           // 加入房间
    P2P_MSG_LEAVE_ROOM,          // 离开房间
    P2P_MSG_ROOM_JOINED,         // 房间加入成功
    P2P_MSG_ROOM_LEFT,           // 房间离开成功
    P2P_MSG_OFFER,               // 发送 offer
    P2P_MSG_ANSWER,              // 发送 answer
    P2P_MSG_CANDIDATE,           // 发送 ICE candidate
    P2P_MSG_PROBE_REQUEST,       // 网络探测请求
    P2P_MSG_PROBE_RESPONSE,      // 网络探测响应
    P2P_MSG_STREAM_REQUEST,      // 请求流
    P2P_MSG_STREAM_READY,        // 流准备就绪
    P2P_MSG_ERROR,               // 错误消息
    P2P_MSG_UNKNOWN              // 未知消息类型
} P2PMessageType;

typedef struct P2PSignalMessage {
    P2PMessageType type;         // 消息类型
    char* id;                    // 发送者ID
    union {
        struct {
            char* room_id;       // 房间ID
            char* role;          // 角色(sender/receiver)
            char* capabilities;  // 能力描述
        } join_room;
        
        struct {
            char* room_id;       // 房间ID
            char* role;          // 角色
            int max_receivers;   // 最大接收者数量
            int current_receivers; // 当前接收者数量
        } room_joined;
        
        struct {
            char* description;   // SDP描述
        } offer;
        
        struct {
            char* description;   // SDP描述
        } answer;
        
        struct {
            char* candidate;     // ICE candidate
            char* mid;           // Media ID
        } candidate;
        
        struct {
            char* probe_id;      // 探测ID
            int packet_size;     // 探测包大小
        } probe_request;
        
        struct {
            char* probe_id;      // 探测ID
            char* status;        // 状态（如"ok", "timeout", "error"等）
            int64_t timestamp;   // 时间戳
            uint32_t crc;        // CRC校验值
        } probe_response;
        
        struct {
            char* error_code;    // 错误代码
            char* error_msg;     // 错误信息
        } error;
        
        struct {
            char* stream_id;     // 流ID
        } stream_request;
        
        struct {
            char* stream_id;     // 流ID
        } stream_ready;
    } payload;
} P2PSignalMessage;

// ==================== 核心信令函数声明 ====================

/**
 * 发送信令消息（统一入口）
 * @param ctx P2P上下文
 * @param msg 信令消息结构体
 * @return 0表示成功，负数表示错误
 */
int p2p_send_signal_message(P2PContext* ctx, const P2PSignalMessage* msg);

/**
 * 处理接收到的信令消息（统一入口）
 * @param ctx P2P上下文
 * @param message JSON格式的信令消息
 * @param size 消息大小
 * @return 0表示成功，负数表示错误
 */
int p2p_handle_signal_message(P2PContext* ctx, const char* message, int size);

// ==================== 工具函数声明 ====================

/**
 * 创建基础信令消息
 * @param type 消息类型
 * @param sender_id 发送者ID
 * @param receiver_id 接收者ID（可为NULL）
 * @param room_id 房间ID（可为NULL）
 * @return 信令消息指针，失败返回NULL
 */
P2PSignalMessage* p2p_create_signal_message(P2PMessageType type, 
                                            const char* sender_id);

/**
 * 释放信令消息内存
 * @param msg 信令消息指针
 */
void p2p_free_signal_message(P2PSignalMessage* msg);

/**
 * 根据枚举值获取消息类型字符串
 * @param type 消息类型枚举值
 * @return 对应的字符串，如果无效则返回"unknown"
 */
const char* p2p_message_type_to_string(P2PMessageType type);

/**
 * 根据字符串获取消息类型枚举值
 * @param type_str 消息类型字符串
 * @return 对应的枚举值，如果无效则返回P2P_MSG_ERROR
 */
P2PMessageType p2p_string_to_message_type(const char* type_str);

/**
 * 设置信令消息处理回调
 * @param ctx P2P上下文
 * @param handler 消息处理回调函数
 * @param user_data 用户自定义数据
 * @return 0表示成功，负数表示错误
 */
int p2p_set_signal_message_handler(struct P2PContext* ctx, 
                                   int (*handler)(struct P2PContext*, const P2PSignalMessage*),
                                   void* user_data);

#endif // P2P_SIGNAL_H 