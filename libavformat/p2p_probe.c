#include "p2p_probe.h"
#include "p2p_signal.h"
#include "rtc/rtc.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// ==================== 工具函数 ====================

// 获取当前时间（微秒）
static int64_t get_current_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// 睡眠函数（毫秒）
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

// 数学工具函数
static double clip_double(double value, double min_val, double max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

static double min_double(double a, double b) {
    return (a < b) ? a : b;
}

static double max_double(double a, double b) {
    return (a > b) ? a : b;
}

static uint32_t min_uint32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static uint32_t max_uint32(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

// ==================== 核心探测函数实现 ====================

void probe_data_init(NetworkQualityMetrics *data) {
    //Todo:xy: 这里是否需要初始化其他变量？
    memset(data, 0, sizeof(NetworkQualityMetrics));
    data->start_time = get_current_time_us();
    data->ice_connectivity_score = -1;
    data->probe_channel_id = -1;
    data->phase = PROBING_SLOW_START;
    data->current_packet_size = PROBE_MIN_PACKET_SIZE;
    data->min_rtt = INFINITY;
    data->max_rtt = 0.0;
}

int create_probe_channel(PeerConnectionNode *node) {
    rtcDataChannelInit dc_init = {0};
    dc_init.reliability.unordered = false;
    dc_init.protocol = "probe";
    
    int dc = rtcCreateDataChannel(node->pc_id, PROBE_CHANNEL_LABEL);
    if (dc <= 0) {
        printf("[P2P-Probe] ERROR: Failed to create probe channel\n");
        return -1;
    }
    
    node->network_quality.probe_channel_id = dc;
    
    // 设置回调函数
    rtcSetUserPointer(dc, node);
    rtcSetOpenCallback(dc, on_probe_channel_open);
    rtcSetClosedCallback(dc, on_probe_channel_closed);
    rtcSetMessageCallback(dc, on_probe_channel_message);
    
    printf("[P2P-Probe] INFO: Probe channel created with ID %d\n", dc);
    return 0;
}

int send_probe_packets(PeerConnectionNode *node) {
    if (node->network_quality.probe_channel_id <= 0) {
        printf("[P2P-Probe] ERROR: Probe channel not initialized\n");
        return -1;
    }

    ProbePacket packet;
    int ret;
    
    // 发送不同大小的数据包来测试带宽
    static const uint32_t packet_sizes[] = {
        64,    // 小包
        512,   // 中包
        1024,  // 大包
        2048,  // 较大包
        4096,  // 大包
        8192   // 最大包
    };
    
    for (int i = 0; i < PROBE_PACKET_COUNT; i++) {
        memset(&packet, 0, sizeof(ProbePacket));
        packet.sequence_number = i;
        packet.send_time = get_current_time_us();
        packet.packet_size = packet_sizes[i % (sizeof(packet_sizes)/sizeof(packet_sizes[0]))];
        packet.phase = node->network_quality.phase;
        
        // 通过探测通道发送数据包
        ret = rtcSendMessage(node->network_quality.probe_channel_id, 
                           (const char*)&packet, 
                           packet.packet_size);
        
        if (ret != RTC_ERR_SUCCESS) {
            printf("[P2P-Probe] ERROR: Failed to send probe packet %d\n", i);
            return -1;
        }
        
        node->network_quality.packets_sent++;
        printf("[P2P-Probe] DEBUG: Sent probe packet %d, size %u\n", i, packet.packet_size);
        sleep_ms(100); // 100ms间隔
    }
    
    printf("[P2P-Probe] INFO: Sent %d probe packets\n", PROBE_PACKET_COUNT);
    return 0;
}

void handle_probe_packet(PeerConnectionNode *node, const char *data, int size) {
    if (size < sizeof(ProbePacket)) {
        return;
    }

    ProbePacket *packet = (ProbePacket *)data;
    NetworkQualityMetrics *metrics = &node->network_quality;
    int64_t now = get_current_time_us();
    
    // 更新统计数据
    metrics->packets_received++;
    metrics->bytes_received += packet->packet_size;
    metrics->last_recv_time = now;
    
    // 计算RTT
    double rtt = (now - packet->send_time) / 1000.0; // 转换为毫秒
    metrics->rtt_sum += rtt;
    metrics->rtt_count++;
    
    // 检测拥塞
    detect_congestion(metrics, rtt);
    
    // 计算丢包率
    if (metrics->packets_sent > 0) {
        metrics->packet_loss_rate = 1.0 - ((double)metrics->packets_received / metrics->packets_sent);
    }
    
    // 计算带宽 (kbps)
    double duration = (now - metrics->start_time) / 1000000.0; // 转换为秒
    if (duration > 0) {
        metrics->bandwidth_kbps = (metrics->bytes_received * 8.0 / 1024.0) / duration;
        
        // 更新探测阶段
        update_probing_phase(metrics);
        
        // 调整下一个包的大小
        adjust_packet_size(metrics);
    }
    
    printf("[P2P-Probe] DEBUG: Received packet %u, RTT=%.2f ms, BW=%.2f kbps\n", 
           packet->sequence_number, rtt, metrics->bandwidth_kbps);
}

double calculate_node_score(NetworkQualityMetrics *data) {
    if (data->packets_received == 0) {
        return 0.0;
    }
    
    double rtt_score = 0.0;
    double packet_loss_score = 0.0;
    double bandwidth_score = 0.0;
    double ice_score = 0.0;
    
    // RTT得分 (0-25分)
    double avg_rtt = data->rtt_sum / data->rtt_count;
    rtt_score = 25.0 * (1.0 - (avg_rtt / MAX_RTT_MS));
    rtt_score = clip_double(rtt_score, 0.0, 25.0);
    
    // 丢包率得分 (0-25分)
    packet_loss_score = 25.0 * (1.0 - (data->packet_loss_rate / MAX_PACKET_LOSS));
    packet_loss_score = clip_double(packet_loss_score, 0.0, 25.0);
    
    // 带宽得分 (0-25分)
    bandwidth_score = 25.0 * (data->bandwidth_kbps / MIN_BANDWIDTH_KBPS);
    bandwidth_score = clip_double(bandwidth_score, 0.0, 25.0);
    
    // ICE连接状态得分 (0-25分)
    ice_score = data->ice_connectivity_score * 25.0 / 100.0;
    ice_score = clip_double(ice_score, 0.0, 25.0);
    
    // 总分 (0-100分)
    data->final_score = rtt_score + packet_loss_score + bandwidth_score + ice_score;
    
    printf("[P2P-Probe] INFO: Node score calculation - RTT:%.1f, Loss:%.1f, BW:%.1f, ICE:%.1f, Total:%.1f\n",
           rtt_score, packet_loss_score, bandwidth_score, ice_score, data->final_score);
    
    return data->final_score;
}

PeerConnectionNode* select_best_node(P2PContext *ctx) {
    PeerConnectionNode *curr = ctx->peer_connection_node_caches;
    PeerConnectionNode *best_node = NULL;
    double best_score = 0.0;
    
    printf("[P2P-Probe] INFO: Selecting best node from available candidates\n");
    
    while (curr) {
        double score = calculate_node_score(&curr->network_quality);
        printf("[P2P-Probe] INFO: Node %s score: %.2f\n", curr->remote_id, score);
        
        if (score > best_score) {
            best_score = score;
            best_node = curr;
        }
        curr = curr->next;
    }
    
    if (best_node) {
        printf("[P2P-Probe] INFO: Selected node %s with score %.2f\n", 
               best_node->remote_id, best_score);
    } else {
        printf("[P2P-Probe] WARNING: No suitable node found\n");
    }
    
    return best_node;
}

// ==================== 探测通道回调函数实现 ====================

void on_probe_channel_open(int dc, void *ptr) {
    PeerConnectionNode *node = ptr;
    printf("[P2P-Probe] INFO: Probe channel %d opened for node %s\n", dc, node->remote_id);
}

void on_probe_channel_closed(int dc, void *ptr) {
    PeerConnectionNode *node = ptr;
    printf("[P2P-Probe] INFO: Probe channel %d closed for node %s\n", dc, node->remote_id);
}

void on_probe_channel_message(int dc, const char *msg, int size, void *ptr) {
    PeerConnectionNode *node = ptr;
    printf("[P2P-Probe] DEBUG: Received message on probe channel %d, size %d\n", dc, size);
    handle_probe_packet(node, msg, size);
}

// ==================== 高级探测算法函数实现 ====================

void update_probing_phase(NetworkQualityMetrics *data) {
    int64_t now = get_current_time_us();
    double duration = (now - data->start_time) / 1000000.0; // 转换为秒
    
    switch (data->phase) {
        case PROBING_SLOW_START:
            // 如果带宽增长率开始下降，或者已经达到目标带宽，进入带宽估计阶段
            if (data->bandwidth_kbps >= TARGET_BANDWIDTH_KBPS ||
                (data->packets_received > 5 && 
                 data->bandwidth_kbps < data->peak_bandwidth_kbps * 0.8)) {
                data->phase = PROBING_BANDWIDTH;
                data->peak_bandwidth_kbps = data->bandwidth_kbps;
                printf("[P2P-Probe] INFO: Phase transition: SLOW_START -> BANDWIDTH\n");
            }
            break;
            
        case PROBING_BANDWIDTH:
            // 如果带宽稳定或者发生拥塞，进入验证阶段
            if (data->congestion_count > 0 ||
                (data->packets_received > 10 && 
                 fabs(data->bandwidth_kbps - data->peak_bandwidth_kbps) < 
                 data->peak_bandwidth_kbps * 0.1)) {
                data->phase = PROBING_VERIFICATION;
                data->stable_bandwidth_kbps = data->bandwidth_kbps;
                printf("[P2P-Probe] INFO: Phase transition: BANDWIDTH -> VERIFICATION\n");
            }
            break;
            
        case PROBING_VERIFICATION:
            // 如果验证阶段的带宽与稳定带宽相差不大，完成探测
            if (data->packets_received > 15 && 
                fabs(data->bandwidth_kbps - data->stable_bandwidth_kbps) < 
                data->stable_bandwidth_kbps * 0.1) {
                data->phase = PROBING_COMPLETE;
                printf("[P2P-Probe] INFO: Phase transition: VERIFICATION -> COMPLETE\n");
            }
            break;
            
        default:
            break;
    }
}

int adjust_packet_size(NetworkQualityMetrics *data) {
    switch (data->phase) {
        case PROBING_SLOW_START:
            // 慢启动阶段，指数增加包大小
            data->current_packet_size = min_uint32(
                data->current_packet_size * 2,
                PROBE_MAX_PACKET_SIZE
            );
            break;
            
        case PROBING_BANDWIDTH:
            // 带宽估计阶段，线性增加包大小
            data->current_packet_size = min_uint32(
                data->current_packet_size + PROBE_MIN_PACKET_SIZE,
                PROBE_MAX_PACKET_SIZE
            );
            break;
            
        case PROBING_VERIFICATION:
            // 验证阶段，使用较大的固定包大小
            data->current_packet_size = PROBE_MAX_PACKET_SIZE * 3 / 4;
            break;
            
        default:
            return -1;
    }
    
    return 0;
}

void detect_congestion(NetworkQualityMetrics *data, double current_rtt) {
    int64_t now = get_current_time_us();
    
    // 更新RTT统计
    if (data->rtt_count == 0) {
        data->min_rtt = data->max_rtt = current_rtt;
    } else {
        data->min_rtt = min_double(data->min_rtt, current_rtt);
        data->max_rtt = max_double(data->max_rtt, current_rtt);
    }
    
    // 检测拥塞
    bool is_congested = false;
    
    // 条件1: RTT显著增加
    if (current_rtt > data->min_rtt * 2) {
        is_congested = true;
    }
    
    // 条件2: 丢包率超过阈值
    if (data->packet_loss_rate > MAX_PACKET_LOSS) {
        is_congested = true;
    }
    
    // 条件3: 带宽突然下降
    if (data->bandwidth_kbps < data->peak_bandwidth_kbps * 0.7) {
        is_congested = true;
    }
    
    if (is_congested) {
        // 如果距离上次拥塞超过1秒
        if (now - data->last_congestion_time > 1000000) {
            data->congestion_count++;
            data->last_congestion_time = now;
            
            // 拥塞发生时减小包大小
            data->current_packet_size = max_uint32(
                data->current_packet_size / 2,
                PROBE_MIN_PACKET_SIZE
            );
            
            printf("[P2P-Probe] WARNING: Congestion detected, count=%d\n", data->congestion_count);
        }
    }
}

double estimate_available_bandwidth(NetworkQualityMetrics *data) {
    if (data->phase != PROBING_COMPLETE) {
        return 0.0;
    }
    
    // 使用稳定带宽作为基准
    double estimated_bw = data->stable_bandwidth_kbps;
    
    // 根据RTT波动调整
    if (data->max_rtt > 0) {
        double rtt_ratio = data->min_rtt / data->max_rtt;
        estimated_bw *= rtt_ratio;
    }
    
    // 根据丢包率调整
    estimated_bw *= (1.0 - data->packet_loss_rate);
    
    // 确保不超过峰值带宽
    estimated_bw = min_double(estimated_bw, data->peak_bandwidth_kbps);
    
    // 确保不低于最低带宽要求
    estimated_bw = max_double(estimated_bw, MIN_BANDWIDTH_KBPS);
    
    printf("[P2P-Probe] INFO: Estimated available bandwidth: %.2f kbps\n", estimated_bw);
    return estimated_bw;
} 

// ==================== 探测相关函数 ====================

int handle_probe_request(PeerConnectionNode* node, const char* msg, int size) {
    P2PContext* p2p_ctx = node->p2p_ctx;
    av_log(p2p_ctx->avctx, AV_LOG_DEBUG, 
           "Handling probe request from receiver %s\n", node->remote_id);
    
    // 创建探测通道（如果还没有）
    if (node->network_quality.probe_channel_id <= 0) {
        int dc = rtcCreateDataChannel(node->pc_id, PROBE_CHANNEL_LABEL);
        if (dc <= 0) {
            av_log(p2p_ctx->avctx, AV_LOG_ERROR, "Failed to create probe channel\n");
            return AVERROR_EXTERNAL;
        }
        node->network_quality.probe_channel_id = dc;
        rtcSetUserPointer(dc, node);
    }
    
    // 开始发送探测数据包
    return send_probe_packets(node);
}

int send_probe_request_to_publisher(P2PContext* p2p_ctx, PeerConnectionNode* node) {
    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_PROBE_REQUEST, node->remote_id);
    if (!msg) {
        return AVERROR(ENOMEM);
    }
    
    // 生成探测ID
    char probe_id[64];
    snprintf(probe_id, sizeof(probe_id), "probe_%08x", av_get_random_seed());
    
    msg->payload.probe_request.probe_id = strdup(probe_id);
    msg->payload.probe_request.packet_size = 1024;
    
    int ret = p2p_send_signal_message(p2p_ctx, msg);
    
    av_log(p2p_ctx->avctx, AV_LOG_INFO, "Sent probe request to %s, probe_id=%s\n", 
           node->remote_id, probe_id);
    
    p2p_free_signal_message(msg);
    return ret;
}

int send_stream_request(P2PContext* p2p_ctx, PeerConnectionNode* node) {
    if (!node) {
        av_log(p2p_ctx->avctx, AV_LOG_ERROR, "No selected node for stream request\n");
        return AVERROR(EINVAL);
    }
    
    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_STREAM_REQUEST, node->remote_id);
    if (!msg) {
        return AVERROR(ENOMEM);
    }
    
    msg->payload.stream_request.stream_id = strdup("main_stream");
    
    int ret = p2p_send_signal_message(p2p_ctx, msg);
    
    av_log(p2p_ctx->avctx, AV_LOG_INFO, "Sent stream request to %s\n", node->remote_id);
    
    p2p_free_signal_message(msg);
    return ret;
} 