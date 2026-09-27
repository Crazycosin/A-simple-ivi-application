# 视频音频流处理设计

```

video file(audio file, h264 packet or buffer, acc packet or buffer) -> mediaplayer -> video_stream -> rgb->connector,plane
                          -> audio_stream - >pcm->asound devices
```