/*
 * WebRTC-HTTP egress protocol (WHEP) demuxer using libdatachannel
 *
 * Copyright (C) 2023 NativeWaves GmbH <contact@nativewaves.com>
 * This work is supported by FFG project 47168763.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/random_seed.h"
#include "version.h"
#include "rtsp.h"
#include "webrtc.h"

// 1. WHEPContext 结构体
// 功能：存储 WHEP 协议的上下文信息
typedef struct WHEPContext {
    const AVClass *av_class;          // FFmpeg 类信息
    DataChannelContext data_channel;  // WebRTC 数据通道上下文
} WHEPContext;

// 2. 函数：whep_read_header
// 功能：读取 WHEP 文件头，初始化 WebRTC 连接和轨道
static int whep_read_header(AVFormatContext* avctx)
{
    WHEPContext*const ctx = (WHEPContext*const)avctx->priv_data;
    int ret, i;
    char media_stream_id[37] = { 0 }; // 媒体流 ID（UUID 格式）
    rtcTrackInit track_init;          // WebRTC 轨道初始化配置
    AVDictionary* options = NULL;     // 输入格式选项
    const AVInputFormat* infmt;       // 输入格式
    AVStream* stream;                 // 流信息
    FFIOContext sdp_pb;               // SDP 文件的 IO 上下文

    webrtc_init_logger(); // 初始化 WebRTC 日志记录器

    // 2.1 初始化 WebRTC 连接
    ret = webrtc_init_connection(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize connection\n");
        goto fail;
    }

    // 2.2 生成媒体流 ID
    ret = webrtc_generate_media_stream_id(media_stream_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        goto fail;
    }

    // 2.3 分配轨道内存
    ctx->data_channel.tracks = av_mallocz(2 * sizeof(DataChannelTrack));
    ctx->data_channel.nb_tracks = 2;
    ctx->data_channel.avctx = avctx;
    if (!ctx->data_channel.tracks) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    for (i = 0; i < ctx->data_channel.nb_tracks; i++) {
        ctx->data_channel.tracks[i].avctx = avctx;
    }

    // 2.4 配置视频轨道
    memset(&track_init, 0, sizeof(rtcTrackInit));
    track_init.direction = RTC_DIRECTION_RECVONLY; // 仅接收方向
    track_init.codec = RTC_CODEC_H264; // 默认视频编解码器
    track_init.payloadType = 96;       // 负载类型
    track_init.ssrc = av_get_random_seed(); // 随机生成 SSRC
    track_init.mid = "0";              // 媒体 ID
    track_init.name = LIBAVFORMAT_IDENT; // 轨道名称
    track_init.msid = media_stream_id; // 媒体流 ID
    track_init.trackId = av_asprintf("%s-video", media_stream_id); // 轨道 ID
    track_init.profile = "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1"; // 视频配置

    // 2.5 添加视频轨道
    ctx->data_channel.tracks[0].track_id = rtcAddTrackEx(ctx->data_channel.peer_connection, &track_init);
    if (!ctx->data_channel.tracks[0].track_id) {
        av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    // 2.6 配置音频轨道
    memset(&track_init, 0, sizeof(rtcTrackInit));
    track_init.direction = RTC_DIRECTION_RECVONLY; // 仅接收方向
    track_init.codec = RTC_CODEC_OPUS; // 默认音频编解码器
    track_init.payloadType = 97;       // 负载类型
    track_init.ssrc = av_get_random_seed(); // 随机生成 SSRC
    track_init.mid = "1";              // 媒体 ID
    track_init.name = LIBAVFORMAT_IDENT; // 轨道名称
    track_init.msid = media_stream_id; // 媒体流 ID
    track_init.trackId = av_asprintf("%s-audio", media_stream_id); // 轨道 ID
    track_init.profile = "minptime=10;maxaveragebitrate=96000;stereo=1;sprop-stereo=1;useinbandfec=1"; // 音频配置

    // 2.7 添加音频轨道
    ctx->data_channel.tracks[1].track_id = rtcAddTrackEx(ctx->data_channel.peer_connection, &track_init);
    if (!ctx->data_channel.tracks[1].track_id) {
        av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    // 2.8 创建 WebRTC 资源
    ret = webrtc_create_resource(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "webrtc_create_resource failed\n");
        goto fail;
    }

    // 2.9 初始化每个轨道的 SDP 复用器
    for (int i = 0; i < ctx->data_channel.nb_tracks; i++) {
        char sdp_track[SDP_MAX_SIZE] = { 0 };

        // 2.9.1 获取轨道描述
        ret = rtcGetTrackDescription(ctx->data_channel.tracks[i].track_id, sdp_track, sizeof(sdp_track));
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "rtcGetTrackDescription failed\n");
            goto fail;
        }

        // 2.9.2 初始化 SDP 文件的 IO 上下文
        ffio_init_read_context(&sdp_pb, (uint8_t*)sdp_track, strlen(sdp_track));

        // 2.9.3 查找 SDP 输入格式
        infmt = av_find_input_format("sdp");
        if (!infmt)
            goto fail;

        // 2.9.4 分配 RTP 上下文
        ctx->data_channel.tracks[i].rtp_ctx = avformat_alloc_context();
        if (!ctx->data_channel.tracks[i].rtp_ctx) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ctx->data_channel.tracks[i].rtp_ctx->max_delay = avctx->max_delay;
        ctx->data_channel.tracks[i].rtp_ctx->pb = &sdp_pb.pub;
        ctx->data_channel.tracks[i].rtp_ctx->interrupt_callback = avctx->interrupt_callback;

        // 2.9.5 复制白名单和黑名单
        if ((ret = ff_copy_whiteblacklists(ctx->data_channel.tracks[i].rtp_ctx, avctx)) < 0)
            goto fail;

        // 2.9.6 设置 SDP 选项
        av_dict_set(&options, "sdp_flags", "custom_io", 0);

        // 2.9.7 打开 SDP 输入
        ret = avformat_open_input(&ctx->data_channel.tracks[i].rtp_ctx, "temp.sdp", infmt, &options);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "avformat_open_input failed\n");
            goto fail;
        }

        // 2.9.8 初始化 URL 上下文
        ret = webrtc_init_urlcontext(&ctx->data_channel, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "webrtc_init_urlcontext failed\n");
            goto fail;
        }

        // 2.9.9 打开文件描述符
        ret = ffio_fdopen(&ctx->data_channel.tracks[i].rtp_ctx->pb, ctx->data_channel.tracks[i].rtp_url_context);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ffio_fdopen failed\n");
            goto fail;
        }

        // 2.9.10 复制编解码器参数
        stream = avformat_new_stream(avctx, NULL);
        if (!stream) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_parameters_copy(stream->codecpar, ctx->data_channel.tracks[i].rtp_ctx->streams[0]->codecpar);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "avcodec_parameters_copy failed\n");
            goto fail;
        }
        stream->time_base = ctx->data_channel.tracks[i].rtp_ctx->streams[0]->time_base;
    }

    return 0;

fail:
    webrtc_deinit(&ctx->data_channel);
    return ret;
}

// 3. 函数：whep_read_close
// 功能：关闭 WHEP 资源并释放上下文
static int whep_read_close(AVFormatContext* avctx)
{
    WHEPContext*const ctx = (WHEPContext*const)avctx->priv_data;
    int ret = 0;

    // 3.1 关闭 WebRTC 资源
    ret = webrtc_close_resource(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "webrtc_close_resource failed\n");
    }

    // 3.2 释放 WebRTC 上下文
    webrtc_deinit(&ctx->data_channel);

    return ret;
}

// 4. 函数：whep_read_packet
// 功能：读取 WHEP 数据包
static int whep_read_packet(AVFormatContext* avctx, AVPacket* pkt)
{
    const WHEPContext*const s = (const WHEPContext*const)avctx->priv_data;
    const DataChannelTrack*const track = &s->data_channel.tracks[pkt->stream_index];
    pkt->stream_index = 0;
    return av_read_frame(track->rtp_ctx, pkt);
}


// 5. WHEP 选项定义
#define OFFSET(x) offsetof(WHEPContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    WEBRTC_OPTIONS(DEC, OFFSET(data_channel)),
    { NULL },
};

// 6. WHEP 类定义
static const AVClass whep_demuxer_class = {
    .class_name = "WHEP demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

// 7. WHEP 输入格式定义
const AVInputFormat ff_whep_demuxer = {
    .name             = "whep",
    .long_name        = NULL_IF_CONFIG_SMALL("WebRTC-HTTP egress protocol (WHEP) demuxer"),
    .flags            = AVFMT_NOFILE | AVFMT_EXPERIMENTAL,
    .priv_class       = &whep_demuxer_class,
    .priv_data_size   = sizeof(WHEPContext),
    .read_header      = whep_read_header,
    .read_packet      = whep_read_packet,
    .read_close       = whep_read_close,
};
