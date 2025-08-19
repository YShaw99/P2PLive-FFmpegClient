P2P-FFmpeg-Client README
=============

* 主播推流客户端：`ffmpeg -re -stream_loop -1 -i video.mp4 -strict -2 -f whip "http://host:port/rtc/v1/whip/?app=live&stream=default_room"`
* 观众推流预览客户端：`ffplay -f whep -i "http://host:port/rtc/v1/whep/?app=live&unified_timestamp=1&stream=default_room" -enable_streaming -output_format p2pmuxer -room default_room `
* 观众拉流预览客户端：`ffplay -f p2pdemuxer -i "http://host:port/rtc/v1/whep/?app=live&unified_timestamp=1&stream=default_room" -room default_room`


编译环境：MacOS Arm64, Apple clang version 15.0.0
依赖：libdatachannel
编译参数：`"--enable-gpl --enable-nonfree --enable-debug=3 --enable-libx264 --enable-libdatachannel --disable-optimizations --disable-asm --disable-stripping --arch=arm --extra-ldflags='-L/opt/homebrew/lib -lcjson -ldatachannel -Wl,-rpath' --extra-cxxflags='-std=c++20'"`

## 注：
`unified_timestamp=1`需要配合流媒体服务器支持统一RTP时间戳需求，请参考：[P2PLive-srs](https://github.com/YShaw99/P2PLive-srs)


## Todo：
* 观众侧启播优化——目前srs没有直接从I帧开始返回，以及阻塞式find_stream
* 观众侧卡顿优化——目前存在并行任务导致初期渲染卡顿，同时CDN切换P2P后应暂停CDN流，浪费带宽占用
* 补片时间戳修复——观众侧推流端会重置时间戳，导致观众拉流侧无法使用补片接口。


FFmpeg README
=============

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides means to alter decoded audio and video through a directed graph of connected filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

## Documentation

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## Contributing

Patches should be submitted to the ffmpeg-devel mailing list using
`git format-patch` or `git send-email`. Github pull requests should be
avoided because they are not part of our review process and will be ignored.
