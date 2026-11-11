# Synthetic AAC-LC fixtures

Generated from the mathematical 997 Hz sine generator (no recorded material),
FFmpeg 7.1.5 native AAC encoder, 128 kb/s stereo LC, 0.12 seconds:

```
ffmpeg -f lavfi -i 'sine=frequency=997:sample_rate=48000:duration=0.12' \
  -ac 2 -c:a aac -profile:a aac_low -b:a 128k -f adts sine-48k.aac
```

Repeat with 44100 for sine-44k.aac. Tests strip each ADTS header and feed
raw access units, as the CarPlay 102 stream does. No FFmpeg runtime is used
by the firmware or tests. The synthetic fixtures may be used under CC0-1.0.
