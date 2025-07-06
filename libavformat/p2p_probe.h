#ifndef P2P_PROBE_H
#define P2P_PROBE_H

#include <stdint.h>

#include "avformat.h"
#include "p2p.h"


// ==================== 核心探测函数 ====================

/**
 * 初始化网络质量指标数据结构
 * @param data 网络质量指标结构体指针
 */
void probe_data_init(struct NetworkQualityMetrics *data);

/**
 * 发送探测数据包序列
 * @param node P2P连接节点
 * @return 0表示成功，负数表示错误
 */
int send_probe_packets(struct PeerConnectionNode *node);

/**
 * 处理接收到的探测数据包
 * @param node P2P连接节点
 * @param data 接收到的数据
 * @param size 数据大小
 */
void handle_probe_packet(struct PeerConnectionNode *node, const char *data, int size);

/**
 * 计算节点的综合得分
 * @param data 网络质量指标
 * @return 综合得分(0-100)
 */
double calculate_node_score(AVFormatContext *avctx, struct NetworkQualityMetrics *data);

/**
 * 从所有节点中选择最优节点
 * @param ctx P2P上下文
 * @return 最优节点指针，NULL表示未找到合适节点
 */
struct PeerConnectionNode* select_best_node(struct P2PContext *ctx);

/**
 * 发送探测请求到推流端
 * @param p2p_ctx P2P上下文
 * @param node 目标节点
 * @return 0表示成功，负数表示错误
 */
int send_probe_request_to_publisher(struct P2PContext* p2p_ctx, struct PeerConnectionNode* node);

/**
 * 发送推流请求
 * @param p2p_ctx P2P上下文
 * @param node 目标节点
 * @return 0表示成功，负数表示错误
 */
int send_stream_request(struct P2PContext* p2p_ctx, struct PeerConnectionNode* node);

// 状态设置回调函数类型
typedef void (*p2p_state_callback_t)(void* user_data, int state);


// ==================== 探测通道回调函数 ====================

/**
 * 探测通道打开回调
 * @param dc 数据通道ID
 * @param ptr 用户数据指针(PeerConnectionNode)
 */
void on_probe_channel_open(int dc, void *ptr);

/**
 * 探测通道错误回调
 * @param dc 数据通道ID
 * @param error 错误信息
 * @param ptr 用户数据指针(PeerConnectionNode)
 */
void on_probe_channel_error(int dc, const char *error, void *ptr);
/**
 * 探测通道关闭回调
 * @param dc 数据通道ID
 * @param ptr 用户数据指针(PeerConnectionNode)
 */
void on_probe_channel_closed(int dc, void *ptr);

/**
 * 探测通道消息回调
 * @param dc 数据通道ID
 * @param msg 消息内容
 * @param size 消息大小
 * @param ptr 用户数据指针(PeerConnectionNode)
 */
void on_probe_channel_message(int dc, const char *msg, int size, void *ptr);


#endif // P2P_PROBE_H 