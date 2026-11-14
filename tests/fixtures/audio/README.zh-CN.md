[English](README.md) | [简体中文](README.zh-CN.md)

# 合成 AAC-LC 测试素材

素材由数学 997 Hz 正弦发生器生成（不含录音），使用 FFmpeg 7.1.5 原生 AAC 编码器，128 kb/s 双声道 LC，时长 0.12 秒：

```
ffmpeg -f lavfi -i 'sine=frequency=997:sample_rate=48000:duration=0.12' \
  -ac 2 -c:a aac -profile:a aac_low -b:a 128k -f adts sine-48k.aac
```

以 44100 重复生成 sine-44k.aac。测试去除每个 ADTS 头并提交原始 Access Unit，与 CarPlay 102 流一致。固件和测试均不使用 FFmpeg 运行库。合成测试素材可按 CC0-1.0 使用。
