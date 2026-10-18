# Changelog

What changed in each version, in plain words. The newest version is at the
top. Each GitHub release shows its section from this file.

To ship a new version: add a `## x.y.z` section here, put the same number in
`VERSION`, and push to `main`.

## 0.3.1 (2026-10-18)

- **Fixed (hopefully):** opening the 3DSLink app could freeze the whole 3DS.
  - The app checked on the streaming service with no time limit. If the
    service was slow to answer, the app waited for a minute or more, and
    while it waited even the HOME button didn't respond. Every check now
    gives up after under a second.
  - The regular checks happen in the background, so the app's screens and
    the HOME button never wait on the network.
  - In 0.3.0 the streaming service ran above some of the 3DS's own system
    programs, including the ones that start apps. It now runs at a normal
    priority and still takes a break after every picture.

## 0.3.0 (2026-10-18)

- **Smoother on an Old 3DS.**
  - Each screen is now sent as two halves. On an Old 3DS, two halves can be
    compressed at the same time: one on the system core and one on the
    game's core, using only time the game or HOME Menu leaves unused.
  - A half that didn't change isn't compressed or sent again.
  - "Faster, less detail" is now on by default on an Old 3DS. Turn it off in
    Settings for a sharper but slower picture.
- **Fixed (hopefully):** games didn't stream. 0.2.11 ran the service at the
  lowest priority, so a running game could take all of its time. It now
  runs above the game's background work and takes a break after every
  picture, like ChirunoMod, so both the game and the HOME Menu get their
  share. The second half-screen worker stays at the lowest priority so it
  can never slow a game down.
- If a game's screen can't be found, the log now says how many programs
  were checked, to help track it down.
- "Show stats" also says how many processor cores are working.

## 0.2.11 (2026-10-18)

- **Fixed (hopefully):** app banners stopped showing on the HOME Menu while
  streaming. On an Old 3DS the streaming service shares a processor core with
  the HOME Menu, and it was using all of it. It now lets everything else on
  that core go first and takes a short break after every frame, like the
  proven ChirunoMod module does.
- **Fixed (hopefully):** "Pop out" on a real 3DS. The main tab now hands its
  stream over the moment you click Pop out (and stops checking the 3DS's
  status), so the 3DS has room for the new window. The 3DS also closes
  spare connections Chrome opens but never uses, instead of turning new ones
  away. If it ever is too busy, the window now says so and retries by
  itself instead of staying blank.
- **Faster:** the 3DS copies the next part of the screen while it compresses
  the current one, and checks the network less often while compressing.
- **New setting: "Faster, less detail".** In Settings on the viewer page.
  It sends each screen at half its width, which takes about half the work,
  so the frame rate roughly doubles. Good for games on an Old 3DS.
- **Stats show where the time goes.** Turn on "Show stats" in Settings to
  see how long each frame takes and how busy the 3DS is.

## 0.2.10 (2026-10-18)

- **Fixed (hopefully):** the viewer page loaded but stayed completely blank.
  Two likely causes, both changed:
  - The 3DS closed each connection the moment it finished sending, which can
    cut off the end of the page, so its code never ran. It now waits for the
    browser to finish reading before it hangs up.
  - Copying the screen picture now follows the proven ChirunoMod
    module: it waits for each copy the same way, and maps the 3DS's video
    memory the same way.
- **The page always says something now.** If the picture can't be captured,
  it shows the reason from the 3DS instead of black screens. If the page
  itself didn't load completely, it tells you to reload.
- **More detailed logs.** The Logs tab on the 3DS now says where each screen's
  picture is, the exact error code when copying it fails, and "sending
  pictures" once it works.

## 0.2.9 (2026-10-18)

- **Secret:** on the 3DSLink app, press Up, Up, Down, Down, Left, Right,
  Left, Right, B, A. Turn the sound up. (The sound needs
  `sdmc:/3ds/dspfirm.cdc`, which most modded 3DS systems already have.)

## 0.2.8 (2026-10-18)

- **Fixed (hopefully):** "Service did not answer". The logs showed the 3DS
  rejecting the service's request to start accepting connections
  ("listen ... failed (errno 9)"). The service asked for a queue of 4 waiting
  connections; it now asks for 1, exactly like the proven ChirunoMod module.
  Browsers retry automatically, so nothing else changes for you.

## 0.2.7 (2026-10-18)

- **New bottom screen.** Big buttons you can tap (or press A, X or START):
  Restart streaming, Turn streaming off/on, and Close this app.
- **New "Logs" tab** on the side of the bottom screen (tap it or press R).
  It shows what the app and the streaming service have been doing, so
  problems can be explained. Scroll with the D-pad or the Up/Down buttons;
  "Home" (or L) goes back. If the service doesn't start, the app opens the
  logs for you.
- X now turns streaming back on as well as off.

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
