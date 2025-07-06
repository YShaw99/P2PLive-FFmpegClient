#include "p2p_probe.h"
#include "p2p_signal.h"
#include "rtc/rtc.h"
#include "libavutil/crc.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/random_seed.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "avformat.h"
#include "p2p_dc.h"

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
    memset(data, 0, sizeof(NetworkQualityMetrics));
    data->start_time = get_current_time_us();
    data->ice_connectivity_score = -1;
    data->phase = PROBING_SLOW_START;  // 开始探测
    data->expected_packets = 0;        // 将在发送时设置
    data->timeout_ms = 0;              // 将在probe_request中设置
    data->probe_start_time = 0;        // 将在收到probe_request时设置
    // data->current_packet_size = PROBE_MIN_PACKET_SIZE;  // 注释：动态包大小
    data->min_rtt = INFINITY;
    data->max_rtt = 0.0;
}


int send_probe_packets(PeerConnectionNode *node) {
    if (!node->probe_track || node->probe_track->track_id <= 0) {
        av_log(node->avctx, AV_LOG_ERROR, "[P2P-Probe] Probe track not initialized\n");
        return -1;
    }

    ProbePacket packet;
    int ret;
    
    // 发送不同大小的数据包来测试带宽
    static const uint32_t packet_sizes[] = {
        128,    // 小包
        256,   // 小中包
        512,   // 中包
        1024,  // 大包
        1400   // 接近MTU的包
    };
    
    int total_packet_types = sizeof(packet_sizes)/sizeof(packet_sizes[0]);
    int packets_per_size = 2;  // 每种大小发送n个包
    int total_packets = total_packet_types * packets_per_size;
    
    // 设置预期包数
    node->network_quality.expected_packets = total_packets;
    
    av_log(node->avctx, AV_LOG_INFO, "[P2P-Probe] Starting probe with %d packets (%d types x %d each)\n", 
           total_packets, total_packet_types, packets_per_size);
    
    for (int i = 0; i < total_packets; i++) {
        memset(&packet, 0, sizeof(ProbePacket));
        packet.header.sequence_number = i;
        packet.header.send_time = get_current_time_us();
        
        // 按顺序使用包大小：前4个用64字节，接下来4个用256字节，以此类推
        int size_index = i / packets_per_size;
        packet.header.packet_size = packet_sizes[size_index];
        packet.header.phase = node->network_quality.phase;
        
        // 计算有效负载大小（减去header大小）
        uint32_t payload_size = packet.header.packet_size - sizeof(ProbePacketHeader);
        if (payload_size > sizeof(packet.padding)) {
            payload_size = sizeof(packet.padding);
            packet.header.packet_size = sizeof(ProbePacketHeader) + payload_size;
        }
        
        // 用随机数填充有效负载
        for (uint32_t j = 0; j < payload_size; j++) {
            packet.padding[j] = (char)(rand() % 256);
        }
        
        // 计算有效负载的CRC32校验和
        const AVCRC *crc_table = av_crc_get_table(AV_CRC_32_IEEE);
        uint32_t crc = av_crc(crc_table, 0, (const uint8_t*)packet.padding, payload_size);
        packet.header.payload_crc = crc;
        
        // 发送探测包
        ret = rtcSendMessage(node->probe_track->track_id, 
                           (const char*)&packet, 
                           packet.header.packet_size);
        
        if (ret != RTC_ERR_SUCCESS) {
            av_log(node->avctx, AV_LOG_ERROR, "[P2P-Probe] Failed to send probe packet %d\n", i);
            return -1;
        }
        
        node->network_quality.packets_sent++;
        av_log(node->avctx, AV_LOG_DEBUG, "[P2P-Probe] Sent probe packet %d/%d, size %u bytes (type: %d)\n", 
               i+1, total_packets, packet.header.packet_size, size_index);
        sleep_ms(100);
    }
    
    av_log(node->avctx, AV_LOG_INFO, "[P2P-Probe] Sent all %d probe packets\n", total_packets);
    return 0;
}

void handle_probe_packet(PeerConnectionNode *node, const char *data, int size) {
    ProbePacket *packet = (ProbePacket *)data;
    NetworkQualityMetrics *metrics = &node->network_quality;
    int64_t now = get_current_time_us();
    
    // 验证接收到的数据大小是否至少包含header
    if (size < sizeof(ProbePacketHeader)) {
        av_log(node->avctx, AV_LOG_WARNING, "[P2P-Probe] Received probe packet size %d, min expected %zu\n", 
               size, sizeof(ProbePacketHeader));
        return;
    }
    // 验证接收到的数据大小与header记录是否一致
    if (size != (int)packet->header.packet_size) {
        av_log(node->avctx, AV_LOG_WARNING, "[P2P-Probe] Size mismatch! Received %d bytes, header says %u bytes\n", 
               size, packet->header.packet_size);
        return;
    }
    
    // 验证数据完整性
    uint32_t payload_size = packet->header.packet_size - sizeof(ProbePacketHeader);
    if (payload_size > sizeof(packet->padding)) {
        av_log(node->avctx, AV_LOG_WARNING, "[P2P-Probe] Invalid payload size %u, max %zu\n", 
               payload_size, sizeof(packet->padding));
        abort();
        return;
    }
    
    // 计算接收到的负载的CRC32校验和并验证
    const AVCRC *crc_table = av_crc_get_table(AV_CRC_32_IEEE);
    uint32_t calculated_crc = av_crc(crc_table, 0, (const uint8_t*)packet->padding, payload_size);
    if (calculated_crc != packet->header.payload_crc) {
        av_log(node->avctx, AV_LOG_WARNING, "[P2P-Probe] CRC mismatch! Expected %u, got %u\n", 
               packet->header.payload_crc, calculated_crc);
        // 数据损坏，不计入统计
        abort();
        return;
    }
    
    // 更新统计数据（使用packet_size作为模拟的网络传输大小）
    metrics->packets_received++;
    metrics->bytes_received += packet->header.packet_size;
    metrics->last_recv_time = now;
    
    // 计算RTT
    double rtt = (now - packet->header.send_time) / 1000.0; // 转换为毫秒
    metrics->rtt_sum += rtt;
    metrics->rtt_count++;
    
    // 更新RTT范围
    if (metrics->rtt_count == 1) {
        metrics->min_rtt = metrics->max_rtt = rtt;
    } else {
        metrics->min_rtt = min_double(metrics->min_rtt, rtt);
        metrics->max_rtt = max_double(metrics->max_rtt, rtt);
    }
    
    // 计算丢包率 - 修复：使用expected_packets而不是packets_sent
    if (metrics->expected_packets > 0) {
        metrics->packet_loss_rate = 1.0 - ((double)metrics->packets_received / metrics->expected_packets);
    } else {
        metrics->packet_loss_rate = 0.0; // 没有预期包数时默认为0
    }
    
    // 计算带宽 (kbps)
    double duration = (now - metrics->start_time) / 1000000.0; // 转换为秒
    if (duration > 0) {
        metrics->bandwidth_kbps = (metrics->bytes_received * 8.0 / 1024.0) / duration;
    }
    
    // 检查是否收到了所有预期的包
    if (metrics->expected_packets > 0 && 
        metrics->packets_received >= metrics->expected_packets) {
        metrics->phase = PROBING_COMPLETE;
        av_log(node->avctx, AV_LOG_INFO, "[P2P-Probe] Probing completed! Received %d/%d packets\n",
               metrics->packets_received, metrics->expected_packets);
    }
    
    av_log(node->avctx, AV_LOG_DEBUG, "[P2P-Probe] Received packet %u (%d/%d), RTT=%.2f ms, BW=%.2f kbps, Loss=%.2f%%\n", 
           packet->header.sequence_number, metrics->packets_received, metrics->expected_packets,
           rtt, metrics->bandwidth_kbps, metrics->packet_loss_rate * 100);
}

double calculate_node_score(AVFormatContext *avctx, NetworkQualityMetrics *data) {
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
    
    av_log(avctx, AV_LOG_INFO, "[P2P-Probe] Node score calculation - RTT:%.1f, Loss:%.1f, BW:%.1f, ICE:%.1f, Total:%.1f\n",
           rtt_score, packet_loss_score, bandwidth_score, ice_score, data->final_score);
    
    return data->final_score;
}

PeerConnectionNode* select_best_node(P2PContext *ctx) {
    PeerConnectionNode *curr = ctx->peer_connection_node_caches;
    PeerConnectionNode *best_node = NULL;
    double best_score = 0.0;
    
    av_log(ctx->avctx, AV_LOG_INFO, "[P2P-Probe] Selecting best node from available candidates\n");
    
    while (curr) {
        double score = calculate_node_score(ctx->avctx, &curr->network_quality);
        av_log(ctx->avctx, AV_LOG_INFO, "[P2P-Probe] Node %s score: %.2f\n", curr->remote_id, score);
        
        if (score > best_score) {
            best_score = score;
            best_node = curr;
        }
        curr = curr->next;
    }
    
    if (best_node) {
        av_log(ctx->avctx, AV_LOG_INFO, "[P2P-Probe] Selected node %s with score %.2f\n", 
               best_node->remote_id, best_score);
    } else {
        av_log(ctx->avctx, AV_LOG_WARNING, "[P2P-Probe] No suitable node found\n");
    }
    
    return best_node;
}

// ==================== 探测通道回调函数实现 ====================

void on_probe_channel_open(int dc, void *ptr) {
    PeerConnectionNode *node = ptr;
    PeerConnectionTrack* track = NULL;
    av_log(node->avctx, AV_LOG_INFO, "[P2P-Probe] Probe channel %d opened for node %s\n", dc, node->remote_id);
    
    if ((track = find_peer_connection_track_by_track_id(node->track_caches, dc)) == NULL) {
        track = av_mallocz(sizeof(PeerConnectionTrack));
        if (!track) {
            av_log(node->avctx, AV_LOG_ERROR, "[P2P-Probe] Failed to allocate probe track\n");
            return;
        }

        int ret = setup_probe_common_logic(node, track, dc);
        if (ret < 0) {
            av_log(node->avctx, AV_LOG_ERROR, "[P2P-Probe] Failed to setup probe track for data channel %d\n", dc);
            av_freep(&track);
            return;
        }

        av_log(node->avctx, AV_LOG_INFO, "[P2P-Probe] Created probe track for data channel %d\n", dc);
    }
    
    rtcSetUserPointer(dc, node);
    rtcSetOpenCallback(dc, on_probe_channel_open);
    rtcSetErrorCallback(dc, on_probe_channel_error);
    rtcSetClosedCallback(dc, on_probe_channel_closed);
    rtcSetMessageCallback(dc, on_probe_channel_message);

    if (node && !node->probe_track && track->track_type == PeerConnectionTrackType_ProbeChannel) {
        node->probe_track = track;
    }
}

void on_probe_channel_error(int dc, const char *error, void *ptr) {
    PeerConnectionNode *node = ptr;
    av_log(node->avctx, AV_LOG_INFO, "[P2P-Probe] Probe channel %d happened error for node %s, error:%s\n", dc, node->remote_id, error);
}

void on_probe_channel_closed(int dc, void *ptr) {
    PeerConnectionNode *node = ptr;
    av_log(node->avctx, AV_LOG_INFO, "[P2P-Probe] Probe channel %d closed for node %s\n", dc, node->remote_id);
}

void on_probe_channel_message(int dc, const char *msg, int size, void *ptr) {
    PeerConnectionNode *node = ptr;
    av_log(node->avctx, AV_LOG_DEBUG, "[P2P-Probe] Received message on probe channel %d, size %d\n", dc, size);
    handle_probe_packet(node, msg, size);
} 

// ==================== 探测相关函数 ====================

int send_probe_request_to_publisher(P2PContext* p2p_ctx, PeerConnectionNode* node) {
    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_PROBE_REQUEST, p2p_ctx->local_id, node->remote_id);
    if (!msg) {
        return AVERROR(ENOMEM);
    }

    char probe_id[64];
    snprintf(probe_id, sizeof(probe_id), "probe_%08x", av_get_random_seed());

    static const uint32_t packet_sizes[] = {128, 256, 512, 1024, 1400};
    int total_packet_types = sizeof(packet_sizes)/sizeof(packet_sizes[0]);
    int packets_per_size = 2;
    int expected_packets = total_packet_types * packets_per_size;
    int timeout_ms = PROBE_TIMEOUT_MS;
    
    msg->payload.probe_request.probe_id = strdup(probe_id);
    msg->payload.probe_request.packet_size = 1024;
    msg->payload.probe_request.expected_packets = expected_packets;
    msg->payload.probe_request.timeout_ms = timeout_ms;
    
    node->network_quality.expected_packets = expected_packets;
    node->network_quality.timeout_ms = timeout_ms;
    node->network_quality.probe_start_time = get_current_time_us();  // 记录探测开始时间
    
    int ret = p2p_send_signal_message(p2p_ctx, msg);
    
    av_log(p2p_ctx->avctx, AV_LOG_INFO, "Sent probe request to %s, probe_id=%s, expected_packets=%d, timeout=%dms (start_time set)\n", 
           node->remote_id, probe_id, expected_packets, timeout_ms);
    
    p2p_free_signal_message(msg);
    return ret;
}

int send_stream_request(P2PContext* p2p_ctx, PeerConnectionNode* node) {
    if (!node) {
        av_log(p2p_ctx->avctx, AV_LOG_ERROR, "No selected node for stream request\n");
        return AVERROR(EINVAL);
    }
    
    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_STREAM_REQUEST, p2p_ctx->local_id, node->remote_id);
    if (!msg) {
        return AVERROR(ENOMEM);
    }
    
    msg->payload.stream_request.stream_id = strdup("main_stream");
    
    int ret = p2p_send_signal_message(p2p_ctx, msg);
    
    av_log(p2p_ctx->avctx, AV_LOG_INFO, "Sent stream request to %s\n", node->remote_id);
    
    p2p_free_signal_message(msg);
    return ret;
} 