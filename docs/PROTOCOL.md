# 3DSLink local streaming protocol (draft)

This is the first browser-facing protocol target for 3DSLink.

## Transport

The 3DS acts as a tiny HTTP server on the local network.

Default port: `8080`

Example viewer URL:

```text
http://192.168.1.47:8080/
```

## Endpoints

- `/` - embedded browser viewer
- `/stream/top.mjpg` - top-screen MJPEG stream
- `/stream/bottom.mjpg` - bottom-screen MJPEG stream
- `/api/status` - small JSON status object

## v0.1 goals

The first implementation should favor reliability on Old 3DS hardware over maximum image quality.

- Top screen: 400x240
- Bottom screen: 320x240
- JPEG quality: start around 55
- No audio
- Both screens may use different frame pacing
- Browser reconnects automatically if a stream connection drops

## Old 3DS capture strategy

ChirunoMod already captures Old 3DS framebuffers in eight narrow chunks to reduce working-memory requirements. 3DSLink should preserve that low-memory capture strategy internally. The browser-facing side does not need to expose those chunks: the 3DS can initially reconstruct/encode browser-compatible frames, and later revisions can move strip assembly into JavaScript if that proves materially faster.

## Safety

The background process should preserve an emergency shutdown combination: `START + SELECT`.

The streamer should yield aggressively during title launches/transitions on Old 3DS hardware rather than trying to maintain frame rate at all costs.
