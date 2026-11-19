# Home Assistant

## Feeding and sensors: tuya-local

Install [tuya-local](https://github.com/make-all/tuya-local), then add the device with:
- host: the feeder's IP
- device ID and local key: obtained once while the device was online
- protocol: **3.2** (worked in testing; the device announces 3.3)
- device type: the `v330l_petfeeder` config

The feeder accepts only one local connection at a time. Close the Smart Life app on phones on the same LAN if the entities go `unavailable`.

After a feeder reboot, the entities are `unavailable` for about 1 minute while the offline assist releases the firmware.

To feed (the entity names depend on your setup):
```yaml
action: number.set_value
target:
  entity_id: number.<feeder>_manual_feed
data:
  value: 1
```
Schedule meals with an HA automation, not the Smart Life meal plan.

## Camera: through go2rtc

With the streams from [go2rtc.yaml](go2rtc.yaml), add a **Generic Camera** (Settings → Devices & services → Add integration → Generic Camera) with this stream source:
```
rtsp://<go2rtc-host>:8554/feeder_h264
```
Leave "Still image URL" empty, so HA takes snapshots from the stream, and use TCP transport.

Or, if HA's built-in go2rtc or the WebRTC Camera card reads the go2rtc container directly, point it at the `feeder` / `feeder_h264` streams.

Notes:
- The first snapshot after a pause can fail while go2rtc connects; later ones arrive in a few seconds.
- Use `feeder` (H.265) instead of `feeder_h264` if all your viewers can decode HEVC. That saves the transcode.
