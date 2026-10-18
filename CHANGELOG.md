# Changelog

What changed in each version, in plain words. The newest version is at the
top. Each GitHub release shows its section from this file.

To ship a new version: add a `## x.y.z` section here, put the same number in
`VERSION`, and push to `main`.

## 0.2.6 (2026-10-18)

- **Fixed (hopefully):** the streaming service could never open its port and
  kept logging "port 8080 busy (errno 9)". Error 9 actually means the 3DS
  rejected the socket, not that the port was taken. The service now opens the
  port the way the proven HzMod/ChirunoMod modules do: it waits for Wi-Fi,
  binds to the 3DS's own address, and skips an option the 3DS may not like.
  The log now says exactly which step failed.
- **Fixed (hopefully):** "BIBIBI" beeps when 3DSLink is highlighted on the
  HOME Menu. The banner sound was always silent, but every build that beeped
  stored it as 22 kHz mono. It is now 44.1 kHz stereo, the format known to
  work.

## 0.2.5 (2026-10-18)

- **Fixed:** with 0.2.4, highlighting 3DSLink on the HOME Menu showed no
  banner and froze the 3DS. The 3D banner is removed and the flat banner is
  back.
- Every release now starts with a changelog, so you can see what changed.

## 0.2.4 (2026-10-18)

- **New:** a 3D HOME Menu banner. Highlight 3DSLink and turn up the 3D
  slider: the 3DS and laptop screens lean back, the Wi-Fi waves pulse towards
  you, and the logo floats in front.

## 0.2.3 (2026-10-18)

- **New look for the 3DS app.** A big status line ("Ready to stream!",
  "Waiting for Wi-Fi", …), numbered steps, and the address in large text.
  The bottom screen explains the buttons in plain words.
- **Removed** the QR code from the 3DS screen.
- **Fixed:** if your 3DS's address ended in .128 to .255 (for example
  192.168.1.200), the app said "Not connected to Wi-Fi".
- **Fixed:** the START + SELECT emergency stop could trigger by itself on
  some systems. It now needs exactly those two buttons.

## 0.2.2 (2026-10-18)

- **Fixed:** the QR code on the 3DS screen flickered constantly.
- If the streaming service doesn't start, the app now shows why on the bottom
  screen. The service keeps a log at `sdmc:/3ds/3DSLink/log.txt`.

## 0.2.1 (2026-10-18)

- **Fixed:** opening 3DSLink crashed the console (Luma3DS "loader"
  exception) while it started the streaming service. The service is now
  packaged the same way as other proven background modules. The app replaces
  the broken copy automatically.

## 0.2.0 (2026-10-18)

- Rebuilt from scratch, replacing the 0.1.0 alpha builds.
- One CIA to install. The app sets up its background streaming service
  itself.
- Streams both screens to Chrome, with layouts (3DS, side by side, top,
  bottom), a pop-out window for Discord, fullscreen, and quality and frame
  rate settings.
- Uses little memory, so it can run alongside games on an Old 3DS. A slow
  viewer skips frames instead of falling behind, and a still screen isn't
  resent.
- New HOME Menu icon and banner. The banner sound is silent.
