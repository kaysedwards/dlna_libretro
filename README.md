# DLNA libretro core

A libretro core (for RetroArch and other libretro frontends) that browses
DLNA / UPnP media servers on your network (MiniDLNA/ReadyMedia, Jellyfin,
Plex, Serviio, Universal Media Server, ...) and plays their video, music
and pictures, all with a game controller.

* Lists every media server on the network, with the server's own icon.
* Browses folders as a grid of thumbnails, using the thumbnails the server
  provides (or downloading and shrinking the picture when it has none).
* Plays anything FFmpeg can decode, with subtitle and audio track
  selection, seeking, A-B looping, pause and five aspect ratio modes.
* Can start directly in a folder, or play a file, given a
  `dlna://server/folder/file` path.
* Plays playlists (`.m3u`, `.dlna`, `.json`) of media from DLNA servers,
  local files, HTTP/HTTPS, SFTP and SMB shares, in order or shuffled, once
  or repeating.
* A spectrum visualization over the cover art while music plays.
* Colours, background images and the thumbnail frame style can all be
  changed.
* Save states remember what was playing (by its `dlna://` path), the
  position, the audio and subtitle tracks and the A-B loop.
* Written in ISO C90 (see `docs/C90_NOTES.md`).

Contents: [Controls](#controls) - [Core options](#core-options) -
[Installing](#installing) - [Using it](#using-it) -
[Playlists](#playlists) - [Save states](#save-states) -
[The system/dlna folder](#the-systemdlna-folder) - [Building](#building) -
[Troubleshooting](#troubleshooting) - [For maintainers](#for-maintainers)

## Controls

Face buttons are described by *position* because the letters differ
between controllers. "South" is Xbox **A**, PlayStation **Cross**,
RetroPad **B**.

| Button (default) | In the browser | During playback |
|------------------|----------------|-----------------|
| D-Pad | Move the selection | Up/Down: subtitle track, Left/Right: audio track (swappable) |
| South | Open folder / play | - |
| East | Back to the parent folder | Stop and return to the browser |
| West | - | Show the title (again to hide) |
| North | - | Show / hide the playback information panel |
| L2 / R2 (triggers) | Page up / page down | Seek back / forward |
| L1 / R1 (shoulders) | - | Set A-B loop points (in a playlist: previous / next entry) |
| Select | - | Cycle aspect ratio |
| Start | Search for servers again / reload the folder | Pause / resume |

Every face button assignment, the D-Pad assignment, Start/Select and
shoulders/triggers can be changed in the core options.

**Moving around the browser wraps**: Down on the bottom row continues at
the top (same column), Up on the top row continues at the bottom, Left on
the first item goes to the last and Right on the last to the first. A page
down (R2) from the very last item jumps back to the start, and a page up
(L2) from the first item to the end. Arrows above and below the
thumbnails show when the list continues off screen.

**Seeking.** Each press of a trigger jumps 10 seconds. Pressing again
(in the same direction) within 1.5 seconds jumps 30 seconds, then 60
seconds, then 5 minutes, and keeps jumping 5 minutes for as long as you
keep pressing. Holding the trigger repeats. Pause for 1.5 seconds and it
starts again at 10 seconds.

**A-B loop.** Press L1 at the start of the part you want (point A) and R1
at the end (point B). Playback jumps back to A and repeats A to B
seamlessly. Press either shoulder to stop looping. (Pressing them in the
other order works too, as long as A ends up before B.)

**Aspect ratio** cycles through: *Media Selected* (what the file says),
*Forced Full Screen* (stretch), *16:9*, *4:3* and *5:4*.

**Subtitles** cycle through *Off* and every subtitle track in the file.
Text subtitles (SRT, ASS, WebVTT, ...) and picture subtitles (Blu-ray PGS,
DVD, DVB) are supported. ASS styling and positioning are not; ASS lines
are shown as plain text.

## Core options

Found in RetroArch under *Quick Menu > Core Options*.

| Option | Values | Default |
|--------|--------|---------|
| Resolution | 640x480 ... 3840x2160 | 1280x720 |
| Font Size | 16 - 72 pixels | 28 |
| Subtitle Size | 75% - 200% | 100% |
| Initial Subtitles | Default/Forced Track, Off, First Track | Default/Forced |
| Open / Play Button | South, East, North, West | South |
| Back / Stop Button | South, East, North, West | East |
| Show Title Button | South, East, North, West | West |
| Playback Info Button | South, East, North, West | North |
| D-Pad During Playback | Up/Down: Subtitles, Left/Right: Audio (or the reverse) | Up/Down subtitles |
| Swap Start and Select | on / off | off |
| Swap Shoulders and Triggers | on / off (swaps L1 with L2 and R1 with R2) | off |
| Sort Order | Alphabetical (A-Z), Reverse (Z-A) | A-Z |
| Folder Placement | Folders First, Mixed With Media | Folders First |
| Playback > Ordering | Normal, Random | Normal |
| Playback > Looping | Individual Selection, Playlist Once, Repeat Entry, Repeat All | Playlist Once |
| Playback > Picture Duration | 3 seconds - 5 minutes | 10 seconds |
| Title Panel Border | Transparent, Image, colours | Light Blue |
| Title Panel Text | colours | White |
| Title Panel Background | Transparent, Image, colours | Charcoal |
| Info Panel Border | Transparent, Image, colours | White |
| Info Panel Text | colours | White |
| Info Panel Background | Transparent, Image, colours | Navy |
| Selection Box Border | Image, colours | Light Blue |
| Selection Box Background | Transparent, Image, colours | Slate |
| Navigation Background | Image, colours | Midnight |
| Playback Background | Image, colours | Black |
| Visualization | Transparent, colours | Light Blue |
| Panel Opacity | 50% - 100% | 85% |
| Border Image Corner | 4 - 32 pixels | 8 |
| Border Image Side | Match Corner, 4 - 256 pixels | Match Corner |
| Border Image Repeat | Fit, Clip | Fit |
| Scroll Arrows | Transparent, Image, colours | Light Blue |
| Scroll Arrow Opacity | 25% - 100% | 60% |

Notes:

* **Resolution** is the size of the canvas the core draws on. A libretro
  core cannot see your window, so pick the one with the same shape as your
  screen; RetroArch scales it. The browser shows 1 to 4 thumbnails across,
  as many as fit at no less than 256x256 pixels with padding, and as many
  rows as fit above the title panel. (1024x768 gives 3 across, 1280x720
  gives 4.)
* If two face-button options name the same button, the more important
  action keeps it (open > back > title > info) and the other is disabled.
* Sorting compares numbers by value, so "Track 2" comes before "Track 10".
* **Colours.** 24 named colours are offered. *Transparent* draws nothing:
  a transparent panel border leaves the border off, a transparent panel
  background leaves just the text.
* **Selection Box Background** is the square drawn behind every thumbnail.
  With *Transparent* no squares are drawn and the selection frame goes
  tightly around the picture itself (a landscape thumbnail gets a
  landscape frame).
* **Image** backgrounds come from the `system/dlna` folder:
  `information.png`, `title.png`, `selection.png` (stretched to fit),
  `navigation.png` and `playback.png` (scaled to cover the screen and
  cropped). If the file is missing, panels use black or white, whichever
  stands out against their text colour, and the selection box uses the
  one that stands out against its border colour.
* The info panel colours are also used for the short messages that pop
  up (seeking, track changes, ...).
* **Visualization** draws spectrum bars over the cover art (or the music
  icon) while audio plays; *Transparent* turns it off.
* **Border images.** Setting a border to *Image* draws it from
  `system/dlna/borders.png` as a "nine-slice": squares of *Border Image
  Corner* pixels are cut from the four corners and drawn as they are, and
  pieces *Border Image Side* pixels long (and *Corner* pixels thick) are
  cut from the middle of each edge and repeated along the sides. With
  *Border Image Repeat* set to *Fit* the edge pieces are stretched or
  squeezed slightly so a whole number of them fits between the corners,
  so the only joins are the ones you drew; *Clip* draws them at their real
  size and cuts the last one off, which keeps pixel art exact but usually
  leaves a seam next to the right (or bottom) corner. An image
  made for exactly these sizes is Corner + Side + Corner pixels square:
  with a 12-pixel corner and a 96-pixel side, `borders.png` is 120x120,
  the top and bottom pieces are 96x12 and the left and right ones 12x96.
  A bigger image works too; the extra part in the middle of each edge is
  left out. *Match Corner* makes the edge pieces squares the size of the
  corners. *Corner* is also the border's thickness: at 1280x720 one image
  pixel is one screen pixel; larger canvases scale it up. Without
  `borders.png` (or if it is smaller than Corner + Side + Corner) the
  border is drawn in a solid colour (the panel's text colour, or one that
  stands out against the thumbnail boxes).
* **Scroll arrows** use the room left between the thumbnails, the top of
  the screen and the title panel, up to 2/5 of the title panel's height,
  so they never cost a row of thumbnails. At 1280x720 with the default font
  that leaves a slim 10-pixel strip; bigger canvases get bigger arrows.
  With *Image*, `arrows/backward.png` is drawn at the top and
  `arrows/forward.png` at the bottom, scaled to fit and centred.
* **Playback** options apply to playlists; see [Playlists](#playlists).

## Installing

1. Copy `dlna_libretro.so` (`.dll` on Windows, `.dylib` on macOS) into
   RetroArch's cores directory (*Settings > Directory > Cores*; on Linux
   usually `~/.config/retroarch/cores`).
2. Optionally copy `dlna_libretro.info` into the core info directory
   (*Settings > Directory > Core Info*). Without it RetroArch still loads
   the core but shows it by file name and may not offer *Start Core*.
3. Optionally create the folder `dlna` inside RetroArch's system directory
   (*Settings > Directory > System/BIOS*) and copy the icons from
   `assets/dlna/` into it. See [The system/dlna folder](#the-systemdlna-folder).

## Using it

### Browse every server

*Main Menu > Load Core > DLNA*, then *Start Core*. From the command line:

```sh
retroarch -L ~/.config/retroarch/cores/dlna_libretro.so
```

Servers appear as they answer (usually within a second). Choose one and
you get *Audio*, *Picture* and *Video*; choose one of those to browse that
part of the server.

### Start somewhere specific: dlna:// paths

```
dlna://SERVER
dlna://SERVER/FOLDER
dlna://SERVER/FOLDER/SUBFOLDER/.../FILE.EXT
```

* **SERVER** can be the server's full name ("subaru: minidlna"), the part
  before a colon ("subaru"), its address ("192.168.1.109" or
  "192.168.1.109:5080") or its UDN. Case does not matter.
* **Only a server**: you start at that server's *Audio / Picture / Video*
  choice.
* **A folder**: the first name is looked for *anywhere* on the server
  (using the server's search function when it has one, otherwise by
  browsing), and every further name must be a subfolder of the previous
  one. The folder becomes your *top folder*: Back will not go above it.
  The server's own top level folders count too, so
  `dlna://subaru/Video/System` names MiniDLNA's Video view explicitly.
* **The same folder name in several places** is normal (MiniDLNA, for
  example, has a "Favorites" folder for each media directory). The core
  checks what each one contains and picks the one with video, then audio,
  then pictures. Among equals, a folder directly at the server's top level
  wins, then the first the server reports.
* **A file**: the extension decides which kind of media is preferred
  (`.mkv` = video, `.flac` = audio, `.jpg` = picture, with common
  extension lists for each). Files match by title (with or without the
  extension) or by the file name in the server's URL. A file name alone
  (`dlna://subaru/Some Song.mp3`) is searched for anywhere. The file plays
  at once; stopping it leaves you in its folder.
* Spaces may be written as they are or as `%20`.

Examples, for a MiniDLNA server called "subaru: minidlna":

```
dlna://subaru
dlna://subaru/System
dlna://subaru/Video/System/Hatsune Miku - Levan Polka.mp4
dlna://subaru/Hatsune Miku - Levan Polka.mp4
```

Characters that have a meaning in the path are written as `%XX` inside a
name: `%2F` for `/`, `%25` for `%`, `%3F` for `?`, `%23` for `#`.

There are two ways to hand such a path to RetroArch:

1. **A playlist file** (recommended): an `.m3u` file containing the path,
   for example `living-room.m3u` containing `dlna://subaru/System`. Open
   it with the DLNA core like any other content; it can go in RetroArch's
   playlists and favourites. A playlist with a single folder in it simply
   opens that folder. See [Playlists](#playlists) for more.
2. **The command line**:
   `retroarch -L dlna_libretro.so "dlna://subaru/System"`.

> **RetroArch's built-in media player.** When a path *ends in a media
> extension* (`.mp4`, `.mp3`, `.jpg`, ...), RetroArch ignores `-L` and
> opens it with its own built-in player, which does not know `dlna://`.
> This is RetroArch behaviour that a core cannot change. Either use a
> `.dlna` file, leave the extension off (`dlna://subaru/System/Some Title`
> also works), or turn off *Settings > Multimedia > Use Builtin Media
> Player* (and *Use Builtin Image Viewer* for pictures).

For testing, the core also plays a local file or an `http://` URL given
directly as content.

## Playlists

The core plays three kinds of playlist file. Open them with the DLNA core
like any other content.

**`.m3u` / `.m3u8`**: one entry per line, each a full URI or a path to a
local file. Relative paths are relative to the playlist. Lines starting
with `#` are standard M3U directives or comments (`#EXTM3U`,
`#EXTINF:...`) and are skipped, so playlists saved by other programs work.

```
#EXTM3U
dlna://subaru/Favorites/Test1.mp4
https://example.com/videos/trailer.mp4
Music/local song.mp3
```

**`.dlna`**: the first line is the scheme, the second the host (or
`host:port`), and every further line a file on that host. For `file` the
host line is blank, `localhost` or `127.0.0.1`.

```
dlna
subaru
Favorites/Test1.mp4
Favorites/Test2.mkv
```

(Older `.dlna` files, with a full `dlna://` URI on each line, still work.)

**`.json`**: several hosts and schemes in one file. `scheme` defaults to
`dlna`; `port`, `username` and `password` are optional.

```json
{"playlists": [
  {"scheme": "dlna", "host": "subaru", "port": "5080",
   "media": ["Favorites/Test1.mp4", "Favorites/Test2.mkv"]},
  {"scheme": "sftp", "host": "nas", "username": "me", "password": "!password$",
   "media": ["/srv/videos/holiday.mkv"]},
  {"scheme": "file", "media": ["/home/me/Videos/clip.mp4"]}
]}
```

### Schemes

| Scheme | What | Notes |
|--------|------|-------|
| `dlna` | A file on a DLNA server | Found the same way as `dlna://` paths (see above). `host:port` must match the server's port too. Credentials are ignored. |
| `file` | A local file | Absolute, or relative to the playlist. |
| `http`, `https` | A web server | Streamed and seekable if the server supports range requests (most do). Spaces etc. in paths are encoded for you. HTTPS certificates are verified against the operating system's trusted certificate list (or `system/dlna/ca-certificates.crt` if present, e.g. on Android or Windows). |
| `sftp` | An SSH server | Paths are absolute (`/srv/videos/x.mkv`). Logs in with your SSH agent or keys first, then the password. **The server's host key must already be known**: connect once with `ssh` or `sftp`, or put the key in `system/dlna/known_hosts` (same format as `~/.ssh/known_hosts`, and used instead of it when present). Unknown or changed keys are refused. |
| `smb` | A Windows / Samba share (SMB2 and SMB3) | The path starts with the share name: `share/folder/file.mkv`. |
| `smbs` | The same, with SMB3 encryption required | Refuses servers that cannot encrypt. |

In URIs (in `.m3u` files) credentials go before the host:
`sftp://me:secret@nas:22/srv/x.mkv`. Write characters such as `@`, `:` or
`/` inside a user name or password as `%40`, `%3A`, `%2F`. Passwords are
never written to the log or into save states. Bear in mind they are
stored in plain text in the playlist file itself.

### Playing order

The *Playback* core options decide how a playlist plays:

* **Ordering**: *Normal* plays entries in the order listed, *Random*
  shuffles them (and reshuffles every time round with *Repeat All*,
  never playing the same entry twice in a row).
* **Looping**:
  * *Individual Selection* plays one entry (the first, or a random one),
    then returns to RetroArch's menu.
  * *Playlist Once* plays every entry once, then returns to RetroArch.
  * *Repeat Entry* plays one entry over and over.
  * *Repeat All* plays the whole list over and over.
* **Picture Duration**: how long each picture is shown.

Entries that cannot be played are skipped with a message. Back (East)
ends the playlist and returns to RetroArch. **L1 / R1 skip to the
previous / next entry** (instead of setting A-B loop points): with *Repeat
All* or *Repeat Entry* they wrap around the ends; otherwise "next" on the
last entry ends the playlist and "previous" on the first restarts it. The
info panel shows the position ("Playlist 3 / 10").

A playlist with a **single entry** is treated exactly like that file
opened on its own: the shoulders set A-B loop points, the *Playback*
options do not apply, and a `dlna://` folder simply opens in the browser. Pause, seeking, tracks, the
A-B loop and so on work as usual for each entry.

## Save states

Saving a state (RetroArch: *Quick Menu > Save State*, or the hotkey)
records:

* the `dlna://` path of the media being played, built from the way you
  got there (for example `dlna://subaru/Video/System/Clip.mp4`, or, if the
  core was started at `dlna://subaru/Favorites`, a path starting with
  that), or of the folder being browsed;
* the position, the audio track, the subtitle track, the A and B loop
  points and whether the loop is on, pause, and the aspect mode;
* during a playlist, the position in the playing order.

Loading a state finds the media again through that path, opens it, and
then restores the rest, so it works across sessions and after the
server's internal IDs have changed. A state saved while browsing reopens
that folder (as the top folder).

States work whether the core was started with content or not. RetroArch
names state files after the content, so states made from the server list
all share the name `DLNA` (use the slots to keep several). States are a
position in a stream rather than an exact machine snapshot, so the core
tells RetroArch not to use them for run-ahead or netplay. RetroArch's
*Rewind* has no useful effect with this core (restoring a position less
than a second away is deliberately ignored).

## History playlists

RetroArch keeps separate video, music and picture histories, but gives
cores no way to add to them, and writing RetroArch's playlist files from
a core is not an acceptable practice for a libretro core. So browsing
and playing inside the core is not recorded there.

What RetroArch does record by itself is the *content* the core was
started with: an `.m3u`, `.dlna` or `.json` file goes into the normal
history (and works when picked again). A `dlna://` path ending in a media extension is filed
under the video/music/picture history as the *built-in player*, which
cannot open it; launch such paths from a playlist file instead if you
want usable history entries. Adding proper support would need a new
libretro environment call for cores to submit history entries, which is
a request for the RetroArch developers.

## The system/dlna folder

Everything here is optional. The folder is `dlna` inside RetroArch's
system directory.

| File | Purpose |
|------|---------|
| `folder.png` | Icon for folders |
| `image.png` | Icon for pictures without a thumbnail (and the *Picture* entry) |
| `audio.png` | Icon for music without album art (and the *Audio* entry) |
| `video.png` | Icon for videos without a thumbnail (and the *Video* entry) |
| `server.png` | Icon for servers that have no icon of their own (folder.png is used if this is missing) |
| `font.ttf` or `font.otf` | The font for all text |
| `servers.txt` | Extra servers, one device description URL per line (see Troubleshooting) |
| `information.png`, `title.png`, `selection.png` | Panel and thumbnail box backgrounds, when their background option is *Image* |
| `navigation.png`, `playback.png` | Browser and playback backgrounds, when their option is *Image* |
| `borders.png` | Nine-slice image for borders set to *Image* (Corner + Side + Corner pixels square) |
| `arrows/backward.png`, `arrows/forward.png` | Scroll arrows (top / bottom), when *Scroll Arrows* is *Image* |
| `folders/pictures.png`, `folders/audio.png`, `folders/video.png` | Folder icons inside the Picture, Audio and Video divisions (`folder.png` otherwise) |
| `known_hosts` | Trusted SFTP servers (used instead of `~/.ssh/known_hosts`) |
| `ca-certificates.crt` | Trusted certificate authorities for HTTPS, PEM format (used instead of the system's list) |

Any size works; images are scaled to fit. Ready-made icons are in
`assets/dlna/` (drawn by `tools/make_icons.py`). Without icons the core
uses Unicode symbols (📁 🖼 🎵 🎬 🖥, or simpler ones if your fonts lack
those).

**Fonts.** Without `font.ttf`/`font.otf` the core uses the font that ships
with RetroArch (its `assets` folder: `pkg/osd-font.ttf`, the Ozone or XMB
menu font), and failing that a common system font. Additional system
fonts are used for characters the main font lacks (Japanese titles, the
symbols above). If no font can be loaded at all, a small built-in font
covers plain ASCII.

## Building

You need a C compiler, `make`, `pkg-config` and development files for
**FFmpeg** (libavformat, libavcodec, libswscale, libswresample,
libavutil), **libupnp** (pupnp) and **FreeType**. **libssh** (SFTP) and
**libsmb2** (SMB) are optional: the Makefile uses them when it finds them,
and without them those schemes report that they are not supported.
libretro-common comes from the git submodule.

### Option A: use your system's libraries (quickest)

```sh
# Arch:
sudo pacman -S base-devel ffmpeg libupnp freetype2 libssh libsmb2
# Debian / Ubuntu:
sudo apt install build-essential pkg-config libavformat-dev libavcodec-dev \
     libswscale-dev libswresample-dev libavutil-dev libupnp-dev libfreetype-dev \
     libssh-dev libsmb2-dev
# Fedora:
sudo dnf install gcc make pkgconf ffmpeg-free-devel libupnp-devel freetype-devel \
     libssh-devel libsmb2-devel
# macOS (Homebrew):
brew install pkg-config ffmpeg libupnp freetype libssh libsmb2

git submodule update --init deps/libretro-common
make
```

The core then needs those libraries installed wherever it runs. HTTPS
works if your FFmpeg was built with a TLS library (distribution packages
are).

Some distributions build libsmb2 with Kerberos support but forget to link
Kerberos into it, so it fails to load ("undefined symbol
gss_inquire_sec_context_by_oid"). The Makefile works around this by
linking `libgssapi_krb5` into the core when it is installed.

### Option B: build everything from the submodules (self-contained)

```sh
git submodule update --init --recursive   # fetch all the submodules
./scripts/build-deps.sh   # mbedTLS, libssh, libsmb2, pupnp, FreeType, FFmpeg (slow)
make
```

This links the libraries statically, so the resulting core runs on any
similar system without FFmpeg, libupnp, libssh or libsmb2 installed.
mbedTLS provides encryption for HTTPS and SFTP; this libsmb2 is built
without Kerberos (password logins work). Install `cmake` and
`nasm` first (or run
`FFMPEG_EXTRA="--disable-x86asm" ./scripts/build-deps.sh` without nasm,
at the cost of slower video decoding).

### Other make targets

```sh
make DEBUG=1      # debug build
make clean
make check-c90    # verify the sources are strict C90
make harness      # build the headless test frontend (docs/TESTING.md)
```

Windows (MSYS2/MinGW) and macOS are handled by the Makefile but have not
been tested.

## Troubleshooting

* **No servers appear.** Discovery uses UDP multicast, which some routers,
  Wi-Fi access points, VPNs and firewalls block. Make sure UDP port 1900
  is allowed. As a workaround, put the server's description URL in
  `<system>/dlna/servers.txt`, one per line. For MiniDLNA it is
  `http://ADDRESS:PORT/rootDesc.xml`; other servers print theirs in their
  logs or web page. On machines with several network interfaces the core
  uses the first one; the log line `UPnP ready on ...` shows which.
* **Turn on logging** to see what the core is doing: RetroArch *Settings >
  Logging > Logging Verbosity* (and *Core Logging Level*: Info). Lines
  start with `[DLNA]`.
* **A file will not play.** The reason is shown on screen and logged. The
  core plays the original file, not a transcoded version.
* **Very large folders** load in pages of 200; the progress is shown.
* **4K canvas** is drawn entirely on the CPU, so choose a smaller
  resolution on slow machines; RetroArch scales it up.

## For maintainers

* `docs/ARCHITECTURE.md` - how the code is organised, threads, the player.
* `docs/C90_NOTES.md` - every place where C90 needs a comment.
* `docs/TESTING.md` - testing with the headless harness.
* `docs/GIT_GUIDE.md` - setting the repository and submodules up, and
  day-to-day git.

### Project layout

```
Makefile, link.T        build files
dlna_libretro.info      RetroArch core info file
src/                    the core
tools/harness.c         headless test frontend
tools/make_icons.py     draws assets/dlna/*.png
scripts/build-deps.sh   builds the submodules into deps/prefix
assets/dlna/            default icons
deps/                   git submodules: libretro-common, pupnp, FFmpeg,
                        freetype, mbedtls, libssh, libsmb2
scripts/mbedtls-user-config.h   extra mbedTLS settings (threading)
docs/                   documentation
```

### Licences

This repository's own code carries no licence yet; choose one before
publishing. If you distribute binaries, mind the licences of what is
linked in:

* FFmpeg: LGPL. As built by `build-deps.sh` it is **LGPL version 3**,
  because mbedTLS's Apache 2.0 licence is only compatible with version 3
  (FFmpeg insists on `--enable-version3` with mbedTLS). Without mbedTLS it
  would be LGPL 2.1.
* libssh and libsmb2: LGPL 2.1.
* mbedTLS: Apache 2.0.
* pupnp: BSD. FreeType: FreeType Licence (or GPL 2). libretro-common:
  MIT-style.

The LGPL requires that people can relink the core against their own
versions of those libraries, which is easiest if you publish the
source.
