# The card

How Lossless OS reads a player's card, for anyone putting music on it: by
hand in Finder or Explorer, with rsync or Syncthing, a music manager, or
Lossless Desktop.

## Music: anywhere, any layout

The player doesn't ask for a layout. Every folder on the card that has
songs is an album, however deep:

```
Music/Artist/Album/01 Song.flac      the usual
Album/01 Song.flac                   an album on its own
Artist/2020/Album/CD1/01 Song.flac   years, discs
01 Song.flac                         loose at the top: the album "Songs"
```

- **Tags win.** Title, artist, album artist, album, track number and year
  come from the files' tags (Vorbis comments, ID3v2 / ID3v1, MP4). Folder
  and file names are only used when a file has none: the folder is the
  album, the folder above it the artist (not a year like `2020`, and not a
  generic name like `Music` or `Audio`), and a name like `03 Song.flac`
  gives the track number and title.
- **Discs.** Folders named `CD1`, `Disc 2`, `disk 3` belong to the album
  folder above them: one album, disc by disc.
- **Skipped:** hidden folders (`.lossless`, `.Trashes`, ...),
  `System Volume Information`, `LOST.DIR`, `$RECYCLE.BIN`, `Android`, and
  `Playlists`. Folders more than six deep aren't looked in.
- **Formats:** FLAC, ALAC (`.m4a`), WAV, AIFF, MP3, Ogg Vorbis (`.ogg`,
  `.oga`), Opus (`.opus`), up to 32-bit / 384 kHz, mono or stereo. AAC
  `.m4a` files are left out.
- **Covers:** `cover.jpg` or `folder.jpg` in the album folder, else the
  picture embedded in the first song (FLAC, MP3, M4A). JPEG, baseline (not
  progressive).

## Playlists

`.m3u` and `.m3u8` files in a `Playlists` folder or at the top of the
card. Each line is a song's path: relative, absolute on the card, or from
another computer (Windows or Mac paths); the player matches songs by their
album folder and file name, so paths needn't be exact.

## The player's own folder

`.lossless/` holds the player's settings, where it was, the library cache
and logs. Leave it alone (deleting it only resets the player).

It's shared with Lossless Desktop, and each side keeps to its own files:

| Who | Files in `/.lossless/` |
|---|---|
| The player (Lossless OS) | `state/`, `logs/`, `bluetooth/`, `fonts/`, `player.json` |
| Lossless Desktop | `sync.json` (what it put on the card, and in `found` the songs it recognised there), `sync-scan.json` (the tags it read from music it didn't put there) |

The player never reads or changes Lossless Desktop's files; Lossless
Desktop never touches the player's.

## `/.lossless/player.json`

The player writes this when it starts (and only if something changed), so
a computer can see what's on the card and what plays without asking:

```json
{
  "format": "lossless-player",
  "format_version": 1,
  "os": "Lossless OS",
  "os_version": "0.9",
  "os_build": "b40c943",
  "player": "HiFi Walker H2",
  "music_anywhere": true,
  "music_folder": "Music",
  "music_layout": "Artist/Album",
  "playlists_folder": "Playlists",
  "playlist_extensions": [".m3u", ".m3u8"],
  "audio_formats": ["flac", "alac", "wav", "aiff", "mp3", "vorbis", "opus"],
  "audio_extensions": [".flac", ".m4a", ".wav", ".aif", ".aiff", ".aifc",
                       ".mp3", ".ogg", ".oga", ".opus"],
  "max_sample_rate": 384000,
  "max_bits": 32,
  "max_channels": 2,
  "gapless": true,
  "tags": ["vorbis_comment", "id3v2", "id3v1", "mp4"],
  "cover_files": ["cover.jpg", "folder.jpg"],
  "cover_embedded": true,
  "cover_image": "jpeg_baseline",
  "private_folder": ".lossless"
}
```

- `music_anywhere`: the player finds music anywhere on the card.
  `music_folder` and `music_layout` only suggest where new music could go
  (`Music/Artist/Album/`), to keep a card tidy; music elsewhere plays too.
- `audio_formats` / `audio_extensions`: what plays as it is, so a sync can
  skip converting those.
- Fields may be added; a reader should ignore the ones it doesn't know and
  check `format_version` for changes that aren't additions.
