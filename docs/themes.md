# Themes

A theme is the player's look. Four come built in, and you can make your
own with a text editor.

| Theme | Look |
|---|---|
| Graphite | Dark gray with an orange accent. The default |
| Gallery | Covers first: Albums, Artists (round) and Playlists (four covers in one) as covers you flip through, on the colour of the cover in front, with a menu that slides in on Back; Songs with covers and a big letter; Now Playing and the lock screen tinted by the cover |
| Backlight | An old green LCD, with dark bars |
| Classic | A classic pocket player: a thin title bar, a blue selection bar, chevrons, the cover beside Home and Music, Albums as Covers on black, and its own Now Playing |

The built-in ones are written in the same format as your own (in
`app/themes.c`), so any of them is a starting point.

Pick one in **Settings > Themes**: the one in use says SET, and Play
switches to the selected one.

## Make your own

1. Copy [assets/themes/Template.txt](../assets/themes/Template.txt) to the
   card's `Themes` folder (make the folder if it isn't there). The file
   name is the theme's name: `Themes/Sunset.txt` is called Sunset.
2. Change the colours you want. Leave the rest out.
3. Eject the card, then go to **Settings > Themes**. Your theme is
   in the list after the built-in ones.

Changed the file? Pick another theme and then yours again to see the
change.

## The format

One setting a line, `name = value`. Colours are `#RRGGBB`. Anything after
` #` is a comment, and so is a line starting with `#`.

| Name | What it paints |
|---|---|
| `dark` | `yes` for a dark theme, `no` for a light one. Colours you leave out come from Graphite (dark) or Aluminium (light) |
| `bg` | The background |
| `fg` | Text. The selected row is a mix of `fg` and `bg` |
| `muted` | Second lines (artists), the hint under Settings |
| `faint` | Small print |
| `line` | Lines between rows, the tracks of bars |
| `raised` | Chips (the format on Now Playing) and the options menu |
| `accent` | The playing bar, lamps, the dot by the format |
| `accentInk` | The dot-matrix times and the lock screen clock |
| `accentFg` | Text on the accent |
| `lcd` | Behind the dot-matrix times |
| `lcdDim` | Time left on Now Playing (dark themes; light ones use `faint`) |

These change the look beyond the colours. Leave them out for the usual
soft gray selection, a plain title bar and the plain list.

| Name | What it does |
|---|---|
| `select` | The selected row's colour: a solid bar instead of the soft gray |
| `selectGrad` | With `select`: the bar fades to this colour at the bottom |
| `selectFg` | Text on the selected row |
| `header` | Fills the title bar with this colour |
| `headerGrad` | With `header`: the bar fades to this colour at the bottom |
| `headerFg` | Text and icons on the title bar |
| `chevrons` | `yes` puts a › on rows that open something |
| `home` | `split` puts Home and Music on the left, with a cover on the right: what's playing, or a slideshow of your albums |
| `albums` | `covers` opens Music > Albums as Covers: the albums as covers you flip through |
| `layout` | `gallery` for Gallery's layout: it starts on Albums as covers; Back on those views slides in a menu of Albums, Artists, Songs, Playlists, Now Playing and Settings (Menu goes to Now Playing); the screen takes the colour of the cover in front. Or `classic` for a classic pocket player's layout: a thin title bar with the name in bold and a green battery, short rows, no mini player, Home as Music, Settings, Shuffle Songs and Now Playing, plain track lists, Now Playing with a reflected cover and a volume bar while it changes, Covers on black with Play turning a cover over to its songs, and a padlock instead of the lock screen |

These names are read too, for things to come: `surface`, `hover`,
`accentSoft`, `danger`, `warn`, `ok`, `lcdFg`, `lcdNext`, `lcdPast`,
`keyTop`, `keyBottom`, `keyEdge`. You can leave them out.

A file that can't be read gives Graphite, and a name that's already taken
(a built-in theme, or the same name twice) is skipped.

## Sharing

Made one you like? Open a pull request adding it to `assets/themes/`, with
a photo of the player wearing it.
