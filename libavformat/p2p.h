//
// Created by Shaw on 2025/3/10.
//

#ifndef P2P_H
#define P2P_H

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <libavutil/time.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/random_seed.h>
#include <libavutil/uuid.h>
#include <libavformat/avformat.h>
#include "url.h"
#include "rtc/rtc.h"
#include "cjson/cJSON.h"

// Forward declaration for signal callbacks
struct P2PSignalCallbacks;
struct P2PContext;
struct P2PSignalMessage;

#define SDP_MAX_SIZE 8192
#define P2P_RTP_MAX_PACKET_SIZE 1450
// 带宽探测相关常量
#define PROBE_MIN_PACKET_SIZE 64     // 最小探测包大小(字节)
#define PROBE_MAX_PACKET_SIZE 1500   // 最大探测包大小(字节)，接近MTU大小
#define PROBE_PACKET_COUNT 20        // 每轮探测发送的包数
#define PROBE_TIMEOUT_MS 5000*1000        // 探测超时时间(毫秒)
#define MIN_BANDWIDTH_KBPS 500       // 最低带宽要求(kbps)
#define TARGET_BANDWIDTH_KBPS 2000   // 目标带宽(kbps)
#define MAX_RTT_MS 1000              // 最大可接受RTT(毫秒)
#define MAX_PACKET_LOSS 0.1          // 最大可接受丢包率(0-1)
#define PROBE_CHANNEL_LABEL "probe_channel"  // 探测通道标签
#define P2P_VIDEO_PAYLOAD_TYPE 96
#define P2P_AUDIO_PAYLOAD_TYPE 97
#define P2P_WAITING_RECEIVER_TIMEOUT_SEC 10        // 等待拉流端超时时间(秒)

// 带宽探测阶段
typedef enum ProbingPhase {
    PROBING_SLOW_START,    // 慢启动阶段
    PROBING_BANDWIDTH,     // 带宽估计阶段
    PROBING_VERIFICATION,  // 带宽验证阶段
    PROBING_COMPLETE      // 探测完成
} ProbingPhase;

typedef struct NetworkQualityMetrics {
    int64_t start_time;         // 探测开始时间戳(微秒)
    int64_t last_recv_time;     // 最后一个探测包接收时间戳(微秒)
    int packets_sent;           // 已发送的探测包数量
    int packets_received;       // 已接收的探测包数量
    double rtt_sum;            // RTT总和(毫秒)，用于计算平均RTT
    int rtt_count;             // 成功的RTT测量次数
    uint64_t bytes_received;   // 接收到的总字节数，用于计算带宽
    double packet_loss_rate;   // 丢包率(0-1)
    double bandwidth_kbps;     // 测量的带宽(kbps)
    // double peak_bandwidth_kbps; // 峰值带宽(kbps)
    // double stable_bandwidth_kbps; // 稳定带宽(kbps)
    int ice_connectivity_score; // ICE连接质量得分(0-100)
    double final_score;        // 最终综合评分(0-100)
    ProbingPhase phase;        // 当前探测阶段
    int expected_packets;      // 预期接收的总包数
    int timeout_ms;            // 探测超时时间(毫秒)
    int64_t probe_start_time;  // 探测开始时间戳(微秒)，用于超时计算
    // uint32_t current_packet_size; // 当前使用的数据包大小
    // int64_t last_congestion_time; // 上次发生拥塞的时间
    double min_rtt;            // 最小RTT(毫秒)
    double max_rtt;            // 最大RTT(毫秒)
    // int congestion_count;      // 拥塞次数
} NetworkQualityMetrics;

typedef struct ProbePacketHeader {
    uint32_t sequence_number;   // 序列号，用于检测丢包
    int64_t send_time;         // 发送时间戳，用于计算RTT
    uint32_t packet_size;      // 有效数据大小，用于带宽测试
    ProbingPhase phase;        // 发送时的探测阶段
    uint32_t payload_crc;      // 有效负载的CRC32校验值
} ProbePacketHeader;

typedef struct ProbePacket {
    ProbePacketHeader header;
    char padding[PROBE_MAX_PACKET_SIZE - sizeof(ProbePacketHeader)];
} ProbePacket;

typedef enum PeerConnectionStatus {
    Disconnected = 0,    // 初始状态或连接断开
    Connecting = 1,      // 正在建立连接
    Connected = 2,       // 连接已建立
    NetworkProbing = 3,  // 正在进行网络测试
    Selected = 4,        // 被选中作为主要连接
    Closed = 5,          // 连接已完成所有流程
    Failed = 6           // 连接失败
} PeerConnectionStatus;

typedef enum PeerConnectionTrackType {
    PeerConnectionTrackType_Unknown = 0,  // 未知类型
    PeerConnectionTrackType_Video = 1,    // 视频轨道
    PeerConnectionTrackType_Audio = 2,    // 音频轨道
    PeerConnectionTrackType_DataChannel = 3, // 数据通道
    PeerConnectionTrackType_ProbeChannel = 4 // 探测通道
} PeerConnectionTrackType;

typedef struct P2PCapabilities {
    int max_receivers;
    char **supported_codecs;
    int supported_codecs_count;
    char *preferred_quality;
    int bandwidth_limit_kbps;
} P2PCapabilities;

typedef struct P2PRoomInfo {
    char *room_id;
    char *room_name;
    int64_t created_time;
    int current_senders;
    int current_receivers;
    int max_capacity;
} P2PRoomInfo;

typedef struct P2PStreamConfig {
    char *video_codec;
    char *audio_codec;
    int video_bitrate_kbps;
    int audio_bitrate_kbps;
    int fps;
    char *resolution;
} P2PStreamConfig;
// ====== 结构定义结束 ======

// Track 级别
typedef struct PeerConnectionTrack {
    AVFormatContext *avctx;
    AVFormatContext *rtp_ctx;
    URLContext *rtp_url_context;
    int stream_index;
    PeerConnectionTrackType track_type;
    int track_id;                               // Maybe track or data_channel.

    // ==== Stream level configuration ====
    P2PStreamConfig stream_config;             // 从信令或本地配置获得的流参数

    struct PeerConnectionTrack* next;
} PeerConnectionTrack;

typedef struct PeerConnectionNodeCandidate {
    char candidate[512];
    char mid[32];
    struct PeerConnectionNodeCandidate* next;
} PeerConnectionNodeCandidate;

// User 级别
typedef struct PeerConnectionNode {
    AVFormatContext* avctx;
    struct P2PContext* p2p_ctx;
    const char *remote_id;
    int pc_id;                                  // peer_connection Wrap(function)
    // NetworkQuality NetworkQualityWhenInit;   //xy:Todo:如何动态更新P2P的网络状态？
    // int inited;
    PeerConnectionStatus status;
    PeerConnectionTrack* track_caches;
    PeerConnectionTrack* video_track;
    PeerConnectionTrack* audio_track;
    PeerConnectionTrack* probe_track;
    PeerConnectionNodeCandidate* candidate_caches;
    
    NetworkQualityMetrics network_quality;

    // P2PCapabilities capabilities;              // 该节点能力集，来自信令或探测

    struct PeerConnectionNode* next;
} PeerConnectionNode;

typedef struct P2PContext {
    AVFormatContext* avctx;
    rtcConfiguration* config;
    // 4位唯一id
    char* local_id;                             // local id应该根据本地外网IP和端口号生成，防止重复。

    // 信令服务器
    int web_socket;                             // signal_server_ws
    int web_socket_connected;
    char* web_socket_server_address;
    char* web_socket_server_port;
    // char* web_socket_local_id;
    // char* web_socket_remote_id;

    P2PRoomInfo         room_info;
    PeerConnectionNode* peer_connection_node_caches;
    PeerConnectionNode* selected_node;

    // Added for callbacks
    struct P2PSignalCallbacks* signal_callbacks;
} P2PContext;

typedef struct P2PSignalCallbacks {
    void* user_data;
    int (*on_ws_messaged)(P2PContext* ctx, const struct P2PSignalMessage* msg);
    void (*on_pc_connected)(P2PContext* ctx, PeerConnectionNode* node);
} P2PSignalCallbacks;


void *p2p_main(void *arg);
int p2p_close_resource(P2PContext* const ctx);
int p2p_rtp_init_urlcontext(PeerConnectionTrack * const track);

int append_peer_connection_node_to_list(PeerConnectionNode **head, PeerConnectionNode *new_node);
int remove_peer_connection_node_from_list(PeerConnectionNode **head, const char *remote_id);
int release_peer_connection_node(PeerConnectionNode* node);
PeerConnectionNode* find_peer_connection_node_by_pc_id(PeerConnectionNode *head, int pc_id);
PeerConnectionNode* find_peer_connection_node_by_remote_id(PeerConnectionNode *head, const char *remote_id);

int append_peer_connection_track_to_list(PeerConnectionTrack **head, PeerConnectionTrack *new_track);
int remove_peer_connection_track_from_list(PeerConnectionTrack **head, const int track_id);
int release_peer_connection_track(PeerConnectionTrack* node);
PeerConnectionTrack* find_peer_connection_track_by_track_id(PeerConnectionTrack *head, int track_id);
PeerConnectionTrack* find_peer_connection_track_by_stream_index(PeerConnectionTrack *head, int stream_index);

int append_peer_connection_node_candidate_to_list(PeerConnectionNodeCandidate **head, PeerConnectionNodeCandidate *new_candidate);
int clear_peer_connection_node_candidate_list(PeerConnectionNodeCandidate **head);

int p2p_generate_media_stream_id(char media_stream_id[37]);

#endif //P2P_H
