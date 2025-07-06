//
// Created by Shaw on 2025/3/4.
//
#include "p2p_pc.h"
#include "p2p_dc.h"
#include "p2p_ws.h"
#include <unistd.h>
#include "rtpenc.h"
#include "webrtc.h"
#include "rtsp.h"
#include <libavutil/time.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

#include "p2p_probe.h"

int p2p_init_signal_server(P2PContext* const ctx) {
    rtcConfiguration* config = malloc(sizeof(rtcConfiguration));
    memset(config, 0, sizeof(config));
    char* iceServers[] = {
        "stun:stun.l.google.com:19302"
    };
    config->iceServersCount = sizeof(iceServers) / sizeof(iceServers[0]);
    config->iceServers = (char**)malloc(config->iceServersCount * sizeof(char*));
    if (config->iceServers == NULL) {
    }
    for (int i = 0; i < config->iceServersCount; i++) {
        config->iceServers[i] = strdup(iceServers[i]);
        if (config->iceServers[i] == NULL) {
            abort();
        }
    }

    ctx->config = config;

    const char* web_socket_server_address = "127.0.0.1";//"120.53.223.132";
    const char* web_socket_server_port = "8000";
    init_ws_resource(ctx, web_socket_server_address, web_socket_server_port);

    //我方主动建联的测试
    while (ctx->web_socket_connected == 0) {
        av_usleep(1000); //等待ws连接成功 //xy:ToDo:Temp: 应该改成一个阻塞机制，可以放到回调函数里，设置超时时间。
    }
    return 0;
}

int create_peer_connection(P2PContext* const ctx, const char* remote_id) {
    int peer_connection_id, ret;
    rtcConfiguration* config = ctx->config;

    if (ctx == NULL) {
        return AVERROR_INVALIDDATA;
    }

    if ((peer_connection_id = rtcCreatePeerConnection(config)) <= 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create PeerConnection\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    PeerConnectionNode* pc_node = av_mallocz(sizeof(PeerConnectionNode));
    if (pc_node == NULL) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to allocate PeerConnectionNode\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    pc_node->avctx = ctx->avctx;
    pc_node->status = Disconnected;     //xy:ToDo: 3期实现NetWorkTest状态
    pc_node->pc_id = peer_connection_id;
    pc_node->remote_id = strdup(remote_id); //xy:todo:free
    pc_node->p2p_ctx = ctx;
    av_log(ctx->avctx, AV_LOG_INFO, "init local pc id: %d\n", peer_connection_id);
    append_peer_connection_node_to_list(&ctx->peer_connection_node_caches, pc_node);
    return 0;
fail:
    rtcDeletePeerConnection(peer_connection_id);
    return ret;
}

int init_peer_connection(P2PContext* const ctx, int peer_connection_id) {
    PeerConnectionNode* node = find_peer_connection_node_by_pc_id(ctx->peer_connection_node_caches, peer_connection_id);
    int ret;

    rtcSetUserPointer(peer_connection_id, node);
    // only pc
    ret = rtcSetLocalDescriptionCallback(peer_connection_id, on_pc_local_description_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetLocalDescriptionCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetLocalCandidateCallback(peer_connection_id, on_pc_local_candidate_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetLocalCandidateCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetStateChangeCallback(peer_connection_id, on_pc_state_change_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetStateChangeCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetIceStateChangeCallback(peer_connection_id, on_pc_ice_state_change_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetIceStateChangeCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetGatheringStateChangeCallback(peer_connection_id, on_pc_gathering_state_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetGatheringStateChangeCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetSignalingStateChangeCallback(peer_connection_id, on_pc_signaling_state_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetSignalingStateChangeCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    // pc with dc
    ret = rtcSetDataChannelCallback(peer_connection_id, on_pc_data_channel_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetDataChannelCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    ret = rtcSetTrackCallback(peer_connection_id, on_pc_track_callback);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to rtcSetTrackCallback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    return 0;
fail:
    rtcDeletePeerConnection(peer_connection_id);
    return ret;
}

int send_local_description(P2PContext* const ctx, PeerConnectionNode* const node, char* role) {
    rtcSetLocalDescription(node->pc_id, role);
    P2PSignalMessage* msg = p2p_create_signal_message((strcmp(role, "offer") == 0)? P2P_MSG_OFFER : P2P_MSG_ANSWER, ctx->local_id, node->remote_id);
    msg->payload.offer.description = av_mallocz(4096);
    rtcGetLocalDescription(node->pc_id, msg->payload.offer.description, 4096);

    int ret = p2p_send_signal_message(ctx, msg);
    p2p_free_signal_message(msg);
    return ret;
}

// -------- 辅助结构体 --------
typedef struct MediaCountInfo {
    int total;
    int video;
    int audio;
    int application;
} MediaCountInfo;

// -------- 工具函数 --------
static void compare_media_counts_in_sdp(const char *sdp, PeerConnectionNode *node, MediaCountInfo *expected, MediaCountInfo *actual) {
    AVFormatContext *avctx = node->avctx;
    int total_count = 0;
    int audio = 0, video = 0, application = 0;
    const char *line = sdp;
    if (!sdp || !node || !expected || !actual) return;
    av_log(avctx, AV_LOG_DEBUG, "Analyzing SDP content:\n%s\n", sdp);
    
    memset(expected, 0, sizeof(MediaCountInfo));
    memset(actual, 0, sizeof(MediaCountInfo));
    for (PeerConnectionTrack *curr = node->track_caches; curr; curr = curr->next) {
        if (curr->track_type == PeerConnectionTrackType_ProbeChannel) {
            expected->application++;
        } else if (curr->track_type == PeerConnectionTrackType_Video) {
            expected->video++;
        } else if (curr->track_type == PeerConnectionTrackType_Audio) {
            expected->audio++;
        }
        expected->total++;
    }

    while ((line = strstr(line, "m=")) != NULL) {
        const char *line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);

        char media_type[32] = {0};
        if (sscanf(line, "m=%31s", media_type) == 1) {
            av_log(avctx, AV_LOG_DEBUG, "Found media line: m=%s\n", media_type);

            // 解析不同类型的媒体
            if (strncmp(media_type, "video", 5) == 0) {
                video++;
            } else if (strncmp(media_type, "audio", 5) == 0) {
                audio++;
            } else if (strncmp(media_type, "application", 11) == 0) {
                application++;
            } else {
                // av_log(avctx, AV_LOG_DEBUG, "Unknown media type: %s\n", media_type);
            }

            total_count++;
        }

        line += 2; // 跳过 "m="
    }

    actual->audio = audio;
    actual->video = video;
    actual->application = application;
    actual->total = total_count;

    av_log(avctx, AV_LOG_DEBUG, "Media count summary: Expected(Total=%d, Video=%d, Audio=%d, App=%d) vs Actual(Total=%d, Video=%d, Audio=%d, App=%d)\n",
           expected->total, expected->video, expected->audio, expected->application,
           actual->total, actual->video, actual->audio, actual->application);
}

// -------- peer connect callback(pc only) --------
void on_pc_local_description_callback(int peer_connection_id, const char *sdp, const char *type, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;
    int ret = 0;
    // if (node->inited <= 0) {
    //     return;
    // }
    av_log(ctx->avctx, AV_LOG_DEBUG, "peer_connection(local id: %d with remote id: %s) local description set (type=%s)\n", peer_connection_id, node->remote_id, type);
    return;//不从这里发送，改为主动发送。
    //由于需要创建多个Track，但在创建Peerconnection时
    // MediaCountInfo expected = {0}, actual = {0};
    // compare_media_counts_in_sdp(sdp, node, &expected, &actual);
    //
    // if ((actual.video >= expected.video) && (actual.audio >= expected.audio) && (actual.application >= expected.application) && expected.total != 0) {
        av_log(ctx->avctx, AV_LOG_INFO, "Complete SDP generated with all required tracks, sending %s\n", type);
        
        // 发送完整的SDP
        P2PSignalMessage* msg = NULL;
        if (strcmp(type, "offer") == 0) {
            msg = p2p_create_signal_message(P2P_MSG_OFFER, ctx->local_id, node->remote_id);
        } else if (strcmp(type, "answer") == 0) {
            msg = p2p_create_signal_message(P2P_MSG_ANSWER, ctx->local_id, node->remote_id);
        }
        
        if (!msg) {
            av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create signal message\n");
            return;
        }
        
        msg->payload.answer.description = strdup(sdp);

        ret = p2p_send_signal_message(ctx, msg);
        if (msg) {
            p2p_free_signal_message(msg);
        }
        if (ret < 0) {
            av_log(ctx->avctx, AV_LOG_ERROR, "Failed to send %s message\n", type);
        }
    // } else {
    //     av_log(ctx->avctx, AV_LOG_WARNING, "Incomplete SDP, Missing tracks, NOT sending. Video: need %d, got %d; Audio: need %d, got %d; Application: need %d, got %d\n",
    //            expected.video, actual.video, expected.audio, actual.audio, expected.application, actual.application);
    // }
}

void on_pc_local_candidate_callback(int peer_connection_id, const char *cand, const char *mid, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;
    int ret;
    // if (node->inited <= 0) {
    //     return;
    // }

    av_log(ctx->avctx, AV_LOG_DEBUG, "peer_connection(local id: %d with remote id: %s) local ICE candidate (mid=%s), candidate:%s\n", peer_connection_id, node->remote_id, mid, cand);
    // return;
    // char local_sdp[4096] = {0};
    // ret = rtcGetLocalDescription(peer_connection_id, local_sdp, sizeof(local_sdp));
    // if (ret >= 0 && strlen(local_sdp) > 0) {
    //     MediaCountInfo expected = {0}, actual = {0};
    //
    //     compare_media_counts_in_sdp(local_sdp, node, &expected, &actual);
    //
    //     if ((actual.video >= expected.video) && (actual.audio >= expected.audio) &&
    //         (actual.application >= expected.application) && expected.total != 0) {
    //         av_log(ctx->avctx, AV_LOG_INFO, "Local SDP contains all required tracks when generating ICE candidate\n");
    //     } else {
    //         av_log(ctx->avctx, AV_LOG_DEBUG, "Local SDP incomplete when generating ICE candidate. Video: need %d, got %d; Audio: need %d, got %d; Application: need %d, got %d\n",
    //                expected.video, actual.video, expected.audio, actual.audio, expected.application, actual.application);
    //         return;
    //     }
    // } else {
    //     av_log(ctx->avctx, AV_LOG_DEBUG, "Could not retrieve local SDP description for ICE candidate validation\n");
    //     return;
    // }

    P2PSignalMessage* msg = p2p_create_signal_message(P2P_MSG_CANDIDATE, ctx->local_id, node->remote_id);
    if (!msg) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create candidate signal message\n");
        return;
    }

    msg->payload.candidate.candidate = strdup(cand);
    msg->payload.candidate.mid = strdup(mid);

    ret = p2p_send_signal_message(ctx, msg);
    p2p_free_signal_message(msg);
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to send candidate message\n");
    }
}

static const char* webrtc_get_state_name(const rtcState state) {
    switch (state) {
        case RTC_NEW: return "RTC_NEW";
        case RTC_CONNECTING: return "RTC_CONNECTING";
        case RTC_CONNECTED: return "RTC_CONNECTED";
        case RTC_DISCONNECTED: return "RTC_DISCONNECTED";
        case RTC_FAILED: return "RTC_FAILED";
        case RTC_CLOSED: return "RTC_CLOSED";
        default: return "UNKNOWN";
    }
}

static const char* webrtc_get_ice_state_name(const rtcIceState state) {
    switch (state) {
        case RTC_ICE_NEW: return "RTC_ICE_NEW";
        case RTC_ICE_CHECKING: return "RTC_ICE_CHECKING";
        case RTC_ICE_CONNECTED: return "RTC_ICE_CONNECTED";
        case RTC_ICE_COMPLETED: return "RTC_ICE_COMPLETED";
        case RTC_ICE_FAILED: return "RTC_ICE_FAILED";
        case RTC_ICE_DISCONNECTED: return "RTC_ICE_DISCONNECTED";
        case RTC_ICE_CLOSED: return "RTC_ICE_CLOSED";
        default: return "RTC_ICE_UNKNOWN";
    }
}

static const char* webrtc_get_gathering_state_name(const rtcGatheringState state) {
    switch (state) {
        case RTC_GATHERING_NEW: return "RTC_GATHERING_NEW";
        case RTC_GATHERING_INPROGRESS: return "RTC_GATHERING_INPROGRESS";
        case RTC_GATHERING_COMPLETE: return "RTC_GATHERING_COMPLETE";
        default: return "RTC_GATHERING_UNKNOWN";
    }
}

static const char* webrtc_get_signaling_state_name(const rtcSignalingState state) {
    switch (state) {
        case RTC_SIGNALING_STABLE: return "RTC_SIGNALING_STABLE";
        case RTC_SIGNALING_HAVE_LOCAL_OFFER: return "RTC_SIGNALING_HAVE_LOCAL_OFFER";
        case RTC_SIGNALING_HAVE_REMOTE_OFFER: return "RTC_SIGNALING_HAVE_REMOTE_OFFER";
        case RTC_SIGNALING_HAVE_LOCAL_PRANSWER: return "RTC_SIGNALING_HAVE_LOCAL_PRANSWER";
        case RTC_SIGNALING_HAVE_REMOTE_PRANSWER: return "RTC_SIGNALING_HAVE_REMOTE_PRANSWER";
        default: return "RTC_SIGNALING_UNKNOWN";
    }
}

void on_pc_state_change_callback(int peer_connection_id, rtcState state, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) state changed to %s\n", peer_connection_id, node->remote_id, webrtc_get_state_name(state));

    switch (state) {
        case RTC_NEW:
            node->status = Disconnected;
            break;
        case RTC_CONNECTING:
            node->status = Connecting;
            break;
        case RTC_CONNECTED:
            node->status = Connected;

            av_log(ctx->avctx, AV_LOG_INFO, "P2P connection established with %s\n", node->remote_id);

            if (ctx->signal_callbacks && ctx->signal_callbacks->on_pc_connected) {
                av_log(ctx->avctx, AV_LOG_INFO, "Triggering PC connected callback for %s\n", node->remote_id);
                ctx->signal_callbacks->on_pc_connected(ctx, node);
            }
            break;
        case RTC_DISCONNECTED:
            node->status = Disconnected;
            break;
        case RTC_FAILED:
            node->status = Failed;
            break;
        case RTC_CLOSED:
            node->status = Closed;
            break;
        default:
            node->status = Disconnected;
            break;
    }
    //xy:TODO:optimize: 根据状态进行处理：
    // - 断线重连
    // - 释放资源
    // - 触发上层回调
}

void on_pc_ice_state_change_callback(int peer_connection_id, rtcIceState state, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) ICE state changed to %s\n", peer_connection_id, node->remote_id, webrtc_get_ice_state_name(state));

    // 更新ICE连接状态得分
    //xy:Todo:review网络质量评分系统
    switch (state) {
        case RTC_ICE_NEW:
            node->network_quality.ice_connectivity_score = 0;
            break;
        case RTC_ICE_CHECKING:
            node->network_quality.ice_connectivity_score = 25;
            break;
        case RTC_ICE_CONNECTED:
            node->network_quality.ice_connectivity_score = 75;
            break;
        case RTC_ICE_COMPLETED:
            node->network_quality.ice_connectivity_score = 100;
            break;
        case RTC_ICE_FAILED:
            node->network_quality.ice_connectivity_score = 0;
            break;
        case RTC_ICE_DISCONNECTED:
            node->network_quality.ice_connectivity_score = 10;
            break;
        case RTC_ICE_CLOSED:
            node->network_quality.ice_connectivity_score = 0;
            break;
        default:
            node->network_quality.ice_connectivity_score = 0;
            break;
    }
}

void on_pc_gathering_state_callback(int peer_connection_id, rtcGatheringState state, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) gathering state changed to %s\n", peer_connection_id, node->remote_id, webrtc_get_gathering_state_name(state));
}

void on_pc_signaling_state_callback(int peer_connection_id, rtcSignalingState state, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) signaling state changed to %s\n", peer_connection_id, node->remote_id, webrtc_get_signaling_state_name(state));

    //xy:TODO:optimize 根据信令状态：
    // - 发送 Offer/Answer
    // - 检查是否需要重新协商（Renegotiation）
}

void on_pc_data_channel_callback(int peer_connection_id, int dc, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *ctx = node->p2p_ctx;
    AVFormatContext* avctx = node->avctx;

    av_log(ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) new track received (track_id=%d)\n", peer_connection_id, node->remote_id, dc);

    if (find_peer_connection_track_by_track_id(node->track_caches, dc) == NULL) {
        av_log(avctx, AV_LOG_WARNING, "DataChannel track not found in caches, creating new probe track for dc: %d\n", dc);
        PeerConnectionTrack* probe_track = av_mallocz(sizeof(PeerConnectionTrack));
        setup_probe_common_logic(node, probe_track, dc);
    } else {
        av_log(avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) found the channel in caches! (dc_id=%d)\n", peer_connection_id, node->remote_id, dc);
    }
}

void on_pc_track_callback(int peer_connection_id, int tr, void *ptr) {
    PeerConnectionNode *node = ptr;
    P2PContext *p2p_ctx = node->p2p_ctx;
    int ret = 0;
    av_log(p2p_ctx->avctx, AV_LOG_INFO, "peer_connection(local id: %d with remote id: %s) new track received (track_id=%d)\n", peer_connection_id, node->remote_id, tr);

    rtcDirection direct;
    rtcGetTrackDirection(tr, &direct);
    if (direct == RTC_DIRECTION_SENDONLY) {
        av_log(p2p_ctx->avctx, AV_LOG_INFO, "Track %d direction is sendonly, skip\n", tr);
        return;
    }

        // char ssrc[1024] = {};
        // ret = rtcGetSsrcsForTrack(tr, ssrc, 1024);//这个时候是没有ssrc的
        // av_log(ctx->avctx, AV_LOG_INFO, "ssrc is %s\n", ssrc);

        char sdp[1024] = {};
        ret = rtcGetTrackDescription(tr, sdp, sizeof(sdp));
        av_log(p2p_ctx->avctx, AV_LOG_DEBUG, "Track SDP: %s\n", sdp);

        char codec[1024] = {};
        char buffer[1024] = {};
        ret = rtcGetTrackPayloadTypesForCodec(tr, codec, buffer, sizeof(buffer));
        av_log(p2p_ctx->avctx, AV_LOG_INFO, "Track codec: %s, payloads: %s\n", codec, buffer);

        PeerConnectionTrack *track = NULL;
        if (strcmp(sdp, "m=video") || strstr(codec, "h264") || strstr(codec, "h265") || strstr(codec, "vp8") || strstr(codec, "vp9")) {
            if (!node->video_track) {
                track = av_mallocz(sizeof(PeerConnectionTrack));
                if (!track) {
                    av_log(p2p_ctx->avctx, AV_LOG_ERROR, "Failed to allocate video track\n");
                    return;
                }

                track->track_type = PeerConnectionTrackType_Video;
                track->track_id = tr;
                track->avctx = p2p_ctx->avctx;

                AVStream* video_stream = avformat_new_stream(p2p_ctx->avctx, NULL);
                if (video_stream) {
                    video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                    if (strstr(codec, "h264")) {
                        video_stream->codecpar->codec_id = AV_CODEC_ID_H264;
                    } else if (strstr(codec, "h265") || strstr(codec, "hevc")) {
                        video_stream->codecpar->codec_id = AV_CODEC_ID_H265;
                    } else if (strstr(codec, "vp8")) {
                        video_stream->codecpar->codec_id = AV_CODEC_ID_VP8;
                    } else if (strstr(codec, "vp9")) {
                        video_stream->codecpar->codec_id = AV_CODEC_ID_VP9;
                    } else {
                        video_stream->codecpar->codec_id = AV_CODEC_ID_H264; // 默认H264
                        av_log(p2p_ctx->avctx, AV_LOG_WARNING, "Unknown codec type: %s， default H264\n", codec);
                    }
                    track->stream_index = video_stream->index;

                    if ((ret = init_recv_track_ex(node->avctx, video_stream, node, track, PeerConnectionTrackType_Video, tr)) < 0) {
                        av_log(p2p_ctx->avctx, AV_LOG_ERROR, "Failed to initialize video track: %s\n", av_err2str(ret));
                        av_freep(&track);
                        return;
                    }
                }

                node->video_track = track;
                av_log(p2p_ctx->avctx, AV_LOG_INFO, "Created video track for %s (track_id=%d)\n", node->remote_id, tr);
            }
        } else if (strcmp(sdp, "m=audio")  || strstr(codec, "opus") || strstr(codec, "aac") || strstr(codec, "pcm")) {
            if (!node->audio_track) {
                track = av_mallocz(sizeof(PeerConnectionTrack));
                if (!track) {
                    av_log(p2p_ctx->avctx, AV_LOG_ERROR, "Failed to allocate audio track\n");
                    return;
                }

                track->track_type = PeerConnectionTrackType_Audio;
                track->track_id = tr;
                track->avctx = p2p_ctx->avctx;

                AVStream* audio_stream = avformat_new_stream(p2p_ctx->avctx, NULL);
                if (audio_stream) {
                    audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    if (strstr(codec, "opus")) {
                        audio_stream->codecpar->codec_id = AV_CODEC_ID_OPUS;
                    } else if (strstr(codec, "aac")) {
                        audio_stream->codecpar->codec_id = AV_CODEC_ID_AAC;
                    } else if (strstr(codec, "pcm")) {
                        audio_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
                    } else {
                        audio_stream->codecpar->codec_id = AV_CODEC_ID_OPUS; // 默认OPUS
                        av_log(p2p_ctx->avctx, AV_LOG_WARNING, "Unknown codec type: %s， default OPUS\n", codec);
                    }
                    track->stream_index = audio_stream->index;

                    if ((ret = init_recv_track_ex(node->avctx, audio_stream, node, track, PeerConnectionTrackType_Audio, tr)) < 0) {
                        av_log(p2p_ctx->avctx, AV_LOG_ERROR, "Failed to initialize audio track: %s\n", av_err2str(ret));
                        av_freep(&track);
                        return;
                    }
                    // ret = init_recv_track_ex(node->avctx, audio_stream, node, track, PeerConnectionTrackType_Audio, tr);
                    // if (ret < 0) {
                    //     av_log(p2p_ctx->avctx, AV_LOG_ERROR, "Failed to initialize audio track: %s\n", av_err2str(ret));
                    //     av_freep(&track);
                    //     return;
                    // }
                }

                node->audio_track = track;
                av_log(p2p_ctx->avctx, AV_LOG_INFO, "Created audio track for %s (track_id=%d)\n", node->remote_id, tr);
            }
        } else {
            av_log(p2p_ctx->avctx, AV_LOG_WARNING, "Unknown track type for codec: %s\n", codec);

            track = av_mallocz(sizeof(PeerConnectionTrack));
            if (track) {
                track->track_id = tr;
                track->track_type = PeerConnectionTrackType_Unknown;
                track->avctx = p2p_ctx->avctx;
                append_peer_connection_track_to_list(&node->track_caches, track);
            }
        }
    
}
