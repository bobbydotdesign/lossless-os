# Fonts

On the player these live on the card in `/.lossless/fonts`
(`tools/h2-install-test.sh --card-files` copies them).

| File | Font | Licence |
|---|---|---|
| `GeistMono-Medium.ttf`, `GeistMono-SemiBold.ttf` | Geist Mono, by Vercel (the app's face) | SIL Open Font License 1.1 |
| `Geist-*.ttf` | Geist, by Vercel (no longer used by the app) | SIL Open Font License 1.1 |
| `NotoSansCJK-Hangul.otf` | Noto Sans CJK JP Regular 1.002 (Copyright 2014, 2015 Adobe Systems Incorporated), cut down to Korean: Hangul syllables and letters, CJK punctuation, full-width forms | SIL Open Font License 1.1 |

The Korean subset is the fallback for characters Geist doesn't have
(`app/theme.c`). Made with fontTools:

```bash
python3 -m fontTools.subset NotoSansCJKjp-Regular.otf \
    --unicodes="U+AC00-D7A3,U+1100-11FF,U+3130-318F,U+3000-303F,U+FF01-FF5E" \
    --layout-features='' --no-hinting --desubroutinize \
    --output-file=NotoSansCJK-Hangul.otf
```

The licence texts with the fonts' copyright notices are `OFL-Geist.txt`
and `OFL-Noto.txt`; the installers copy them to the card with the fonts.
