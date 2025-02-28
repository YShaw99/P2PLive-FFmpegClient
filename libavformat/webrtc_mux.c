/*
 * WebRTC-HTTP ingestion protocol (WHIP) muxer using libdatachannel
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

#include "avformat.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/time.h"
#include "mux.h"
#include "rtpenc.h"
#include "rtpenc_chain.h"
#include "rtsp.h"
#include "webrtc.h"
#include "version.h"

// 1. WHIPContext 结构体
// 功能：存储 WHIP 协议的上下文信息
typedef struct WHIPContext {
    AVClass *av_class;
    DataChannelContext data_channel;
} WHIPContext;


// 2. 函数：whip_deinit
// 功能：释放 WHIP 上下文资源
static void whip_deinit(AVFormatContext* avctx);

// 3. 函数：whip_init
// 功能：初始化 WHIP 上下文，配置 WebRTC 连接和轨道
static int whip_init(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    AVStream* stream;
    const AVCodecParameters* codecpar;
    int i, ret;
    char media_stream_id[37] = { 0 }; // 媒体流 ID（UUID 格式）
    rtcTrackInit track_init;          // WebRTC 轨道初始化配置
    const AVChannelLayout supported_layout = AV_CHANNEL_LAYOUT_STEREO; // 支持的音频通道布局
    const RTPMuxContext* rtp_mux_ctx; // RTP 复用上下文
    DataChannelTrack* track;          // 数据通道轨道
    char sdp_stream[SDP_MAX_SIZE] = { 0 }; // SDP 流信息
    char* fmtp;                       // SDP 中的 fmtp 参数

    ctx->data_channel.avctx = avctx;
    webrtc_init_logger(); // 初始化 WebRTC 日志记录器

    // 3.1 初始化 WebRTC 连接
    ret = webrtc_init_connection(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize connection\n");
        goto fail;
    }

    // 3.2 分配轨道内存
    if (!(ctx->data_channel.tracks = av_mallocz(sizeof(DataChannelTrack) * avctx->nb_streams))) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate tracks\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    // 3.3 生成媒体流 ID
    ret = webrtc_generate_media_stream_id(media_stream_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        goto fail;
    }

    // 3.4 配置每个轨道
    for (i = 0; i < avctx->nb_streams; ++i) {
        stream = avctx->streams[i];
        codecpar = stream->codecpar;
        track = &ctx->data_channel.tracks[i];

        // 3.4.1 配置视频轨道
        switch (codecpar->codec_type)
        {
            case AVMEDIA_TYPE_VIDEO:
                avpriv_set_pts_info(stream, 32, 1, 90000); // 设置视频时间戳信息
                break;
            case AVMEDIA_TYPE_AUDIO:
                // 3.4.2 检查音频采样率和通道布局
                if (codecpar->sample_rate != 48000) {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate. Only 48kHz is supported\n");
                    ret = AVERROR(EINVAL);
                    goto fail;
                }
                if (av_channel_layout_compare(&codecpar->ch_layout, &supported_layout) != 0) {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported channel layout. Only stereo is supported\n");
                    ret = AVERROR(EINVAL);
                    goto fail;
                }
                avpriv_set_pts_info(stream, 32, 1, codecpar->sample_rate); // 设置音频时间戳信息
                break;
            default:
                continue; // 忽略非音视频轨道
        }

        // 3.4.3 初始化 URL 上下文
        ret = webrtc_init_urlcontext(&ctx->data_channel, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "webrtc_init_urlcontext failed\n");
            goto fail;
        }

        // 3.4.4 打开 RTP 复用器
        ret = ff_rtp_chain_mux_open(&track->rtp_ctx, avctx, stream, track->rtp_url_context, RTP_MAX_PACKET_SIZE, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_rtp_chain_mux_open failed\n");
            goto fail;
        }
        rtp_mux_ctx = (const RTPMuxContext*)ctx->data_channel.tracks[i].rtp_ctx->priv_data;

        // 3.4.5 配置 WebRTC 轨道初始化参数
        memset(&track_init, 0, sizeof(rtcTrackInit));
        track_init.direction = RTC_DIRECTION_SENDONLY; // 仅发送方向
        track_init.payloadType = rtp_mux_ctx->payload_type; // 负载类型
        track_init.ssrc = rtp_mux_ctx->ssrc; // SSRC
        track_init.mid = av_asprintf("%d", i); // 媒体 ID
        track_init.name = LIBAVFORMAT_IDENT; // 轨道名称
        track_init.msid = media_stream_id; // 媒体流 ID
        track_init.trackId = av_asprintf("%s-video-%d", media_stream_id, i); // 轨道 ID

        // 3.4.6 转换编解码器类型
        ret = webrtc_convert_codec(codecpar->codec_id, &track_init.codec);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to convert codec\n");
            goto fail;
        }

        // 3.4.7 解析 SDP 中的 fmtp 参数
        ret = ff_sdp_write_media(sdp_stream, sizeof(sdp_stream), stream, i, NULL, NULL, 0, 0, NULL);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to write sdp\n");
            goto fail;
        }
        fmtp = strstr(sdp_stream, "a=fmtp:");
        if (fmtp) {
            track_init.profile = av_strndup(fmtp + 10, strchr(fmtp, '\r') - fmtp - 10);
            track_init.profile = av_asprintf("%s;level-asymmetry-allowed=1", track_init.profile);
            memset(sdp_stream, 0, sizeof(sdp_stream));
        }

        // 3.4.8 添加 WebRTC 轨道
        track->track_id = rtcAddTrackEx(ctx->data_channel.peer_connection, &track_init);
        if (track->track_id < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }

    return 0;

fail:
    return ret;
}

// 4. 函数：whip_write_header
// 功能：写入 WHIP 文件头，创建 WebRTC 资源并等待连接建立
static int whip_write_header(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    int ret;
    int64_t timeout;

    // 4.1 创建 WebRTC 资源
    ret = webrtc_create_resource(&ctx->data_channel);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create resource\n");
        goto fail;
    }

    // 4.2 等待连接建立
    timeout = av_gettime_relative() + ctx->data_channel.connection_timeout;
    while (ctx->data_channel.state != RTC_CONNECTED) {
        if (ctx->data_channel.state == RTC_FAILED || ctx->data_channel.state == RTC_CLOSED || av_gettime_relative() > timeout) {
            av_log(avctx, AV_LOG_ERROR, "Failed to open connection\n");
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        av_log(avctx, AV_LOG_VERBOSE, "Waiting for PeerConnection to open\n");
        av_usleep(1000); // 等待 1ms
    }

    return 0;

fail:
    return ret;
}

// 5. 函数：whip_write_packet
// 功能：写入 WHIP 数据包
static int whip_write_packet(AVFormatContext* avctx, AVPacket* pkt)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    AVFormatContext* rtpctx = ctx->data_channel.tracks[pkt->stream_index].rtp_ctx;
    pkt->stream_index = 0;

    // 5.1 检查连接状态
    if (ctx->data_channel.state != RTC_CONNECTED) {
        av_log(avctx, AV_LOG_ERROR, "Connection is not open\n");
        return AVERROR(EINVAL);
    }

    // 5.2 写入 RTP 数据包
    return av_write_frame(rtpctx, pkt);
}

// 6. 函数：whip_write_trailer
// 功能：写入 WHIP 文件尾，关闭 WebRTC 资源
static int whip_write_trailer(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    return webrtc_close_resource(&ctx->data_channel);
}

// 7. 函数：whip_deinit
// 功能：释放 WHIP 上下文资源
static void whip_deinit(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    webrtc_deinit(&ctx->data_channel);
}

// 8. 函数：whip_check_bitstream
// 功能：检查比特流，确保关键帧包含 SPS/PPS
static int whip_check_bitstream(AVFormatContext *s, AVStream *st, const AVPacket *pkt)
{
    if (st->codecpar->extradata_size && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return ff_stream_add_bitstream_filter(st, "dump_extra", "freq=keyframe");
    return 1;
}

// 9. 函数：whip_query_codec
// 功能：查询编解码器是否支持
static int whip_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_OPUS:
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_PCM_MULAW:
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_AV1:
        case AV_CODEC_ID_VP9:
            return 1;
        default:
            return 0;
    }
}

// 10. WHIP 选项定义
#define OFFSET(x) offsetof(WHIPContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    WEBRTC_OPTIONS(ENC, OFFSET(data_channel)),
    { NULL },
};

// 11. WHIP 类定义
static const AVClass whip_muxer_class = {
    .class_name = "WHIP muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

// 12. WHIP 输出格式定义
const FFOutputFormat ff_whip_muxer = {
    .p.name             = "whip",
    .p.long_name        = NULL_IF_CONFIG_SMALL("WebRTC-HTTP ingestion protocol (WHIP) muxer"),
    .p.audio_codec      = AV_CODEC_ID_OPUS, // supported by major browsers
    .p.video_codec      = AV_CODEC_ID_H264,
    .p.flags            = AVFMT_NOFILE | AVFMT_GLOBALHEADER | AVFMT_EXPERIMENTAL,
    .p.priv_class       = &whip_muxer_class,
    .priv_data_size     = sizeof(WHIPContext),
    .write_packet       = whip_write_packet,
    .write_header       = whip_write_header,
    .write_trailer      = whip_write_trailer,
    .init               = whip_init,
    .deinit             = whip_deinit,
    .query_codec        = whip_query_codec,
    .check_bitstream    = whip_check_bitstream,
};