/*
 * WebRTC-HTTP ingestion/egress protocol (WHIP/WHEP) common code
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

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/uuid.h"
#include "libavutil/random_seed.h"
#include "rtpenc_chain.h"
#include "rtsp.h"
#include "webrtc.h"

// 1. 函数：webrtc_get_state_name
// 功能：根据 WebRTC 状态返回对应的状态名称字符串
static const char* webrtc_get_state_name(const rtcState state)
{
    switch (state)
    {
        case RTC_NEW:
            return "RTC_NEW";
        case RTC_CONNECTING:
            return "RTC_CONNECTING";
        case RTC_CONNECTED:
            return "RTC_CONNECTED";
        case RTC_DISCONNECTED:
            return "RTC_DISCONNECTED";
        case RTC_FAILED:
            return "RTC_FAILED";
        case RTC_CLOSED:
            return "RTC_CLOSED";
        default:
            return "UNKNOWN";
    }
}

// 2. 函数：webrtc_log
// 功能：将 WebRTC 日志转换为 FFmpeg 日志级别并输出
static void webrtc_log(const rtcLogLevel rtcLevel, const char *const message)
{
    int level = AV_LOG_VERBOSE;
    switch (rtcLevel)
    {
        case RTC_LOG_NONE:
            level = AV_LOG_QUIET;
            break;
        case RTC_LOG_DEBUG:
        case RTC_LOG_VERBOSE:
            level = AV_LOG_DEBUG;
            break;
        case RTC_LOG_INFO:
            level = AV_LOG_VERBOSE;
            break;
        case RTC_LOG_WARNING:
            level = AV_LOG_WARNING;
            break;
        case RTC_LOG_ERROR:
            level = AV_LOG_ERROR;
            break;
        case RTC_LOG_FATAL:
            level = AV_LOG_FATAL;
            break;
    }

    av_log(NULL, level, "[libdatachannel] %s\n", message);
}

// 3. 函数：webrtc_init_logger
// 功能：初始化 WebRTC 日志记录器，将 FFmpeg 日志级别映射到 WebRTC 日志级别
void webrtc_init_logger(void)
{
    rtcLogLevel level = RTC_LOG_VERBOSE;
    switch (av_log_get_level())
    {
        case AV_LOG_QUIET:
            level = RTC_LOG_NONE;
            break;
        case AV_LOG_DEBUG:
            level = RTC_LOG_DEBUG;
            break;
        case AV_LOG_VERBOSE:
            level = RTC_LOG_VERBOSE;
            break;
        case AV_LOG_WARNING:
            level = RTC_LOG_WARNING;
            break;
        case AV_LOG_ERROR:
            level = RTC_LOG_ERROR;
            break;
        case AV_LOG_FATAL:
            level = RTC_LOG_FATAL;
            break;
    }

    rtcInitLogger(level, webrtc_log);
}

// 4. 函数：webrtc_generate_media_stream_id
// 功能：生成唯一的媒体流 ID（UUID 格式）
int webrtc_generate_media_stream_id(char media_stream_id[37])
{
    int ret;
    AVUUID uuid;

    ret = av_random_bytes(uuid, sizeof(uuid));
    if (ret < 0) {
        goto fail;
    }
    av_uuid_unparse(uuid, media_stream_id);
    return 0;

fail:
    return ret;
}

// 5. 函数：webrtc_create_resource
// 功能：创建 WebRTC 资源，发送 SDP 提议并处理服务器响应
int webrtc_create_resource(DataChannelContext*const ctx)
{
    int ret;
    URLContext* h = NULL;
    char* headers;
    char offer_sdp[SDP_MAX_SIZE] = { 0 };
    char response_sdp[SDP_MAX_SIZE] = { 0 };

    /* 1.1 设置本地描述 */
    if (rtcSetLocalDescription(ctx->peer_connection, "offer") != RTC_ERR_SUCCESS) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to set local description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    /* 1.2 获取本地 SDP 提议 */
    ret = rtcGetLocalDescription(ctx->peer_connection, offer_sdp, sizeof(offer_sdp));
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to get local description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(ctx->avctx, AV_LOG_VERBOSE, "offer_sdp: %s\n", offer_sdp);

    /* 1.3 分配 HTTP 上下文 */
    if ((ret = ffurl_alloc(&h, ctx->avctx->url, AVIO_FLAG_READ_WRITE, NULL)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_alloc failed\n");
        goto fail;
    }

    /* 1.4 设置 HTTP 请求头 */
    headers = av_asprintf("Content-type: application/sdp\r\n");
    if (ctx->bearer_token) {
        headers = av_asprintf("%sAuthorization: Bearer %s\r\n", headers, ctx->bearer_token);
    }
    av_log(ctx->avctx, AV_LOG_VERBOSE, "headers: %s\n", headers);
    av_opt_set(h->priv_data, "headers", headers, 0);
    av_opt_set(h->priv_data, "method", "POST", 0);
    av_opt_set_bin(h->priv_data, "post_data", (uint8_t*)offer_sdp, strlen(offer_sdp), 0);

    /* 1.5 打开 HTTP 连接 */
    if ((ret = ffurl_connect(h, NULL)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_connect failed\n");
        goto fail;
    }

    /* 1.6 读取服务器响应 */
    ret = ffurl_read_complete(h, (unsigned char*)response_sdp, sizeof(response_sdp));
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_read_complete failed\n");
        goto fail;
    }

    av_log(ctx->avctx, AV_LOG_VERBOSE, "response: %s\n", response_sdp);

    /* 1.7 设置远程描述 */
    ret = rtcSetRemoteDescription(ctx->peer_connection, response_sdp, "answer");
    if (ret < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to set remote description\n");
        goto fail;
    }

    /* 1.8 保存资源位置 */
    av_opt_get(h->priv_data, "new_location", AV_OPT_SEARCH_CHILDREN, (uint8_t**)&ctx->resource_location);
    av_log(ctx->avctx, AV_LOG_VERBOSE, "resource_location: %s\n", ctx->resource_location);

    /* 1.9 关闭 HTTP 连接 */
    if ((ret = ffurl_closep(&h)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_closep failed\n");
        goto fail;
    }

    av_freep(&headers);
    return 0;

fail:
    if (h) {
        ffurl_closep(&h);
    }
    av_freep(&headers);
    return ret;
}

// 6. 函数：webrtc_close_resource
// 功能：关闭 WebRTC 资源，发送 DELETE 请求释放资源
int webrtc_close_resource(DataChannelContext*const ctx)
{
    int ret;
    URLContext* h = NULL;
    char* headers = NULL;

    if (!ctx->resource_location) {
        return 0;
    }

    /* 2.1 分配 HTTP 上下文 */
    if ((ret = ffurl_alloc(&h, ctx->resource_location, AVIO_FLAG_READ_WRITE, NULL)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_alloc failed\n");
        goto fail;
    }

    /* 2.2 设置 HTTP 请求头 */
    if (ctx->bearer_token) {
        headers = av_asprintf("Authorization: Bearer %s\r\n", ctx->bearer_token);
        av_log(ctx->avctx, AV_LOG_VERBOSE, "headers: %s\n", headers);
    }
    av_opt_set(h->priv_data, "method", "DELETE", 0);

    /* 2.3 打开 HTTP 连接 */
    if ((ret = ffurl_connect(h, NULL)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_connect failed\n");
        goto fail;
    }

    /* 2.4 关闭 HTTP 连接 */
    if ((ret = ffurl_closep(&h)) < 0) {
        av_log(ctx->avctx, AV_LOG_ERROR, "ffurl_close failed\n");
        goto fail;
    }

fail:
    if (h) {
        ffurl_closep(&h);
    }
    av_freep(&ctx->resource_location);
    av_freep(&headers);
    return ret;
}

// 7. 函数：webrtc_read
// 功能：从 WebRTC 数据通道读取数据
static int webrtc_read(URLContext *h, unsigned char *buf, int size)
{
    const DataChannelTrack*const ctx = (const DataChannelTrack*const)h->priv_data;
    int ret;

    ret = rtcReceiveMessage(ctx->track_id, (char*)buf, &size);
    if (ret == RTC_ERR_NOT_AVAIL) {
        return AVERROR(EAGAIN);
    }
    else if (ret == RTC_ERR_TOO_SMALL) {
        return AVERROR_BUFFER_TOO_SMALL;
    }
    else if (ret != RTC_ERR_SUCCESS) {
        av_log(ctx->avctx, AV_LOG_ERROR, "rtcReceiveMessage failed: %d\n", ret);
        return AVERROR_EOF;
    }
    return size;
}

// 8. 函数：webrtc_write
// 功能：向 WebRTC 数据通道写入数据
static int webrtc_write(URLContext *h, const unsigned char *buf, int size)
{
    const DataChannelTrack*const ctx = (const DataChannelTrack*const)h->priv_data;
    int ret;

    ret = rtcSendMessage(ctx->track_id, (const char*)buf, size);
    if (ret != RTC_ERR_SUCCESS) {
        av_log(ctx->avctx, AV_LOG_ERROR, "rtcSendMessage failed: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    return size;
}

// 9. WebRTC URL 协议定义
static const URLProtocol ff_webrtc_protocol = {
    .name            = "webrtc",
    .url_read        = webrtc_read,
    .url_write       = webrtc_write,
};

// 10. 函数：webrtc_init_urlcontext
// 功能：初始化 WebRTC URL 上下文
int webrtc_init_urlcontext(DataChannelContext*const ctx, int track_idx)
{
    DataChannelTrack*const track = &ctx->tracks[track_idx];

    track->rtp_url_context = av_mallocz(sizeof(URLContext));
    if (!track->rtp_url_context) {
        return AVERROR(ENOMEM);
    }

    track->rtp_url_context->prot = &ff_webrtc_protocol;
    track->rtp_url_context->priv_data = track;
    track->rtp_url_context->max_packet_size = RTP_MAX_PACKET_SIZE;
    track->rtp_url_context->flags = AVIO_FLAG_READ_WRITE;
    track->rtp_url_context->rw_timeout = ctx->rw_timeout;
    return 0;
}

// 11. 函数：webrtc_on_state_change
// 功能：WebRTC 连接状态变化回调函数
static void webrtc_on_state_change(int pc, rtcState state, void* ptr)
{
    DataChannelContext*const ctx = (DataChannelContext*const)ptr;

    av_log(ctx->avctx, AV_LOG_VERBOSE, "Connection state changed from %s to %s\n", webrtc_get_state_name(ctx->state), webrtc_get_state_name(state));
    ctx->state = state;
}

// 12. 函数：webrtc_init_connection
// 功能：初始化 WebRTC 连接
int webrtc_init_connection(DataChannelContext *const ctx)
{
    int ret;
    rtcConfiguration config = { 0 };

    if (!(ctx->peer_connection = rtcCreatePeerConnection(&config))) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to create PeerConnection\n");
        return AVERROR_EXTERNAL;
    }

    rtcSetUserPointer(ctx->peer_connection, ctx);

    if (rtcSetStateChangeCallback(ctx->peer_connection, webrtc_on_state_change)) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Failed to set state change callback\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    return 0;

fail:
    rtcDeletePeerConnection(ctx->peer_connection);
    return ret;
}

// 13. 函数：webrtc_convert_codec
// 功能：将 FFmpeg 编解码器 ID 转换为 WebRTC 编解码器类型
int webrtc_convert_codec(enum AVCodecID codec_id, rtcCodec* rtc_codec)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            *rtc_codec = RTC_CODEC_H264;
            break;
        case AV_CODEC_ID_HEVC:
            *rtc_codec = RTC_CODEC_H265;
            break;
        case AV_CODEC_ID_AV1:
            *rtc_codec = RTC_CODEC_AV1;
            break;
        case AV_CODEC_ID_VP9:
            *rtc_codec = RTC_CODEC_VP9;
            break;
        case AV_CODEC_ID_OPUS:
            *rtc_codec = RTC_CODEC_OPUS;
            break;
        case AV_CODEC_ID_AAC:
            *rtc_codec = RTC_CODEC_AAC;
            break;
        case AV_CODEC_ID_PCM_ALAW:
            *rtc_codec = RTC_CODEC_PCMA;
            break;
        case AV_CODEC_ID_PCM_MULAW:
            *rtc_codec = RTC_CODEC_PCMU;
            break;
        default:
            *rtc_codec = -1;
            return AVERROR(EINVAL);
    }

    return 0;
}

// 14. 函数：webrtc_deinit
// 功能：释放 WebRTC 相关资源
void webrtc_deinit(DataChannelContext*const ctx)
{
    if (ctx->tracks) {
        for (int i = 0; i < ctx->nb_tracks; ++i) {
            if (ctx->tracks[i].rtp_ctx)
                avformat_free_context(ctx->tracks[i].rtp_ctx);
            if (ctx->tracks[i].rtp_url_context)
                av_freep(&ctx->tracks[i].rtp_url_context);
            if (ctx->tracks[i].track_id)
                rtcDeleteTrack(ctx->tracks[i].track_id);
        }
        av_freep(&ctx->tracks);
    }
    if (ctx->peer_connection) {
        rtcDeletePeerConnection(ctx->peer_connection);
        ctx->peer_connection = 0;
    }
    if (ctx->resource_location)
        av_freep(&ctx->resource_location);
}