# Part 1: local camera

**Result:** the feeder's camera as a raw H.265 stream on TCP **8556**: 1920×1080, about 10 fps, a keyframe every ~3 s. go2rtc turns it into RTSP/WebRTC for Home Assistant, Frigate or VLC.

It works whether or not the feeder has internet, and leaves the app, cloud and feeding as they were.

**Tested with the app:** with the feeder online, the Smart Life live view (over mobile data and over Wi-Fi) and the local stream played at the same time. A 3-minute local capture during that test had 1,795 frames (≈ 10 fps, nothing dropped), no decode errors, and a keyframe every 3 s.

## How it works

- One program on the feeder, `ak_tuya_ipc`, runs the camera, the Tuya cloud connection and the link to the feeding microcontroller. It can't be replaced or stopped without losing feeding.
- For every video frame it calls `ak_venc_encode_frame()` in Anyka's encoder library (`libmpi_venc.so`). The encoded frame comes back in a small struct (data pointer and length).
- A library preloaded into that program (`LD_PRELOAD`) wraps the call. It runs the real encoder, then copies the main channel's frame to whoever is connected on port 8556.
- When a client connects, it asks the encoder for a keyframe, so video starts within about a second.
- It **never blocks**: if the network is slow, frames are dropped and the stream resumes at the next keyframe. Feeding and the encoder never wait on it.
- It runs no threads and holds no buffers, because the feeder has about 1.7 MB of RAM free.

The stream has no timestamps. go2rtc's ffmpeg adds them; see [examples/go2rtc.yaml](../examples/go2rtc.yaml).

## Facts worth knowing

- **One client at a time:** let go2rtc hold the connection and share it.
- **The keyframe request is shared:** when a local client connects, the keyframe it triggers also goes to any app or cloud viewer. That's a brief bitrate bump, not a glitch.
- **Restarts interrupt the app:** installing or updating the library restarts the stock program, which drops any app session and feeding for about a minute.
- **H.265 only.** Browsers without HEVC support, and Apple HomeKit, need go2rtc's H.264 transcode (`feeder_h264` in the example).
- **There's a second, smaller sub-stream** (encoder channel 1). Select it with `VHOOK_CHN=1`.
- **Per-channel statistics** are written to `/tmp/vhook/stats` every 10 s.
- **Not private on its own:** Part 1 doesn't stop the feeder sending video to Tuya when the app asks. For that, add [Part 2](part2-local-only.md).

## Setup

Follow [install.md](install.md). For the camera only, add `OFFLINE=0` to the config in step 5.
