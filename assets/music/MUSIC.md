# The music

Eleven pieces, about half an hour of music, seventy-two kilobytes in
total. They are Standard MIDI Files, played by `main/audio/midi_seq.c`
through the small synthesiser in `main/audio/midi_synth.c` — see
`main/audio/music.h` for how one is chosen and why the silences between
them are minutes long.

## Two copyrights, not one

A piece of music has **two** copyrights and both have to be clear before
it can ship:

1. **the composition** — Satie died in 1925, Debussy in 1918, Chopin in
   1849, Schumann in 1856 and Bach in 1750, so every composition here is
   long out of copyright everywhere;
2. **the engraving** — the particular typeset edition a MIDI file was
   generated from is a new work with its own copyright, even when the
   music in it is ancient. This is the one that catches people out. A
   MIDI file found loose on the web is almost never licensed for
   redistribution, whoever wrote the tune.

Every file below comes from the [Mutopia Project](https://www.mutopiaproject.org/),
which typesets public-domain scores in LilyPond and publishes the result
under an explicit licence, and **every one of them is Mutopia's "Public
Domain" licence** — not Creative Commons. That was a deliberate choice
when the set was picked: Mutopia also carries a great deal of CC BY-SA
material, including the Gnossiennes, and share-alike would have put
conditions on anyone redistributing SynthMiner. Public domain puts none.

So: these files may be copied, changed, and shipped by anyone, and
nothing in this directory places any obligation on the rest of the game.

## Adding your own

Drop any `.mid` file into `/sd/synthminer/music/` on the badge and it
joins the pool; a file there with the same name as one of ours replaces
it. Nothing needs rebuilding, and nothing here needs deleting first. The
game reads both directories at startup.

Format 0 and format 1 files both work. A file using SMPTE timing is
refused rather than played at an invented speed, and anything over
192 KB is skipped on the assumption that it is not really a MIDI file.

## Regenerating or extending the set

`tools/get_music.py` fetches the list below from Mutopia again and
rewrites this file. It refuses to download anything whose licence is not
Public Domain, so the rule above cannot quietly lapse when the set is
extended:

    python3 tools/get_music.py            # check the shipped files against Mutopia
    python3 tools/get_music.py --survey   # list what else is available, with licences

`make check` parses every file in this directory with the real
sequencer: that it loads, that it has notes in it, that it *ends*, that
rewinding replays it identically, and that every truncation of it still
terminates. A piece added here is covered by that the day it is added.

## What is here

| File | Piece | Composer | Bytes |
|---|---|---|---|
| `bach_sheep_may_safely_graze.mid` | Schafe Können Sicher Weiden (Sheep Will Safely Graze) | J. S. Bach (1685–1750) | 10,217 |
| `chopin_prelude_28_15.mid` | Prelude: Op. 28, No. 15 | F. F. Chopin (1810–1849) | 14,195 |
| `chopin_prelude_28_20.mid` | Prelude: Op. 28, No. 20 | F. F. Chopin (1810–1849) | 2,637 |
| `debussy_arabesque_1.mid` | Première Arabesque | C. Debussy (1862–1918) | 13,154 |
| `debussy_clair_de_lune.mid` | Suite Bergamasque: Clair de Lune | C. Debussy (1862–1918) | 12,476 |
| `satie_gymnopedie_1.mid` | Gymnopédie No. 1 | E. Satie (1866–1925) | 2,892 |
| `satie_gymnopedie_2.mid` | Gymnopédie No. 2 | E. Satie (1866–1925) | 3,613 |
| `satie_gymnopedie_3.mid` | Gymnopédie No. 3 | E. Satie (1866–1925) | 3,340 |
| `schumann_fremde_laender.mid` | Kinderscenen - Von fremden Ländern und Menschen | R. Schumann (1810–1856) | 2,303 |
| `schumann_glueckes_genug.mid` | Kinderscenen - Glückes genug | R. Schumann (1810–1856) | 3,913 |
| `schumann_traeumerei.mid` | Kinderscenen - Traümerei | R. Schumann (1810–1856) | 3,708 |

### Where each one came from

**`bach_sheep_may_safely_graze.mid`** — Schafe Können Sicher Weiden (Sheep Will Safely Graze), J. S. Bach (1685–1750). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=1794>  
File: <https://www.mutopiaproject.org/ftp/BachJS/BWV208/Sheep/Sheep.mid>  
Engraved from: Breitkopf & Härtel, 1881

**`chopin_prelude_28_15.mid`** — Prelude: Op. 28, No. 15, F. F. Chopin (1810–1849). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=471>  
File: <https://www.mutopiaproject.org/ftp/ChopinFF/O28/Chop-28-15/Chop-28-15.mid>  
Engraved from: Edition Peters

**`chopin_prelude_28_20.mid`** — Prelude: Op. 28, No. 20, F. F. Chopin (1810–1849). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=472>  
File: <https://www.mutopiaproject.org/ftp/ChopinFF/O28/Chop-28-20/Chop-28-20.mid>  
Engraved from: Edition Peters

**`debussy_arabesque_1.mid`** — Première Arabesque, C. Debussy (1862–1918). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=1777>  
File: <https://www.mutopiaproject.org/ftp/DebussyC/L66/debussy_Arabesque_1/debussy_Arabesque_1.mid>  
Engraved from: Durand et Fils (1904)

**`debussy_clair_de_lune.mid`** — Suite Bergamasque: Clair de Lune, C. Debussy (1862–1918). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=1778>  
File: <https://www.mutopiaproject.org/ftp/DebussyC/L75/debussy_Ste_Bergamesq_Clair/debussy_Ste_Bergamesq_Clair.mid>  
Engraved from: E. Fromont (1905)

**`satie_gymnopedie_1.mid`** — Gymnopédie No. 1, E. Satie (1866–1925). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=37>  
File: <https://www.mutopiaproject.org/ftp/SatieE/gymnopedie_1/gymnopedie_1.mid>  
Engraved from: Dover Edition

**`satie_gymnopedie_2.mid`** — Gymnopédie No. 2, E. Satie (1866–1925). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=38>  
File: <https://www.mutopiaproject.org/ftp/SatieE/gymnopedie_2/gymnopedie_2.mid>  
Engraved from: Dover Edition

**`satie_gymnopedie_3.mid`** — Gymnopédie No. 3, E. Satie (1866–1925). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=39>  
File: <https://www.mutopiaproject.org/ftp/SatieE/gymnopedie_3/gymnopedie_3.mid>  
Engraved from: Dover Edition

**`schumann_fremde_laender.mid`** — Kinderscenen - Von fremden Ländern und Menschen, R. Schumann (1810–1856). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=354>  
File: <https://www.mutopiaproject.org/ftp/SchumannR/O15/SchumannOp15No01/SchumannOp15No01.mid>  
Engraved from: Leichte Stucke, 1900

**`schumann_glueckes_genug.mid`** — Kinderscenen - Glückes genug, R. Schumann (1810–1856). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=372>  
File: <https://www.mutopiaproject.org/ftp/SchumannR/O15/SchumannOp15No05/SchumannOp15No05.mid>  
Engraved from: Leichte Stucke, 1900

**`schumann_traeumerei.mid`** — Kinderscenen - Traümerei, R. Schumann (1810–1856). Public Domain.  
Mutopia: <https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=504>  
File: <https://www.mutopiaproject.org/ftp/SchumannR/O15/SchumannOp15No07/SchumannOp15No07.mid>  
Engraved from: Leichte Stucke, 1900

## The synth, and what it does to these

They are played by six voice shapes, not a General MIDI sound module:
a struck string, a plucked one, a sustained pad, a bass, a reed and a
bell, chosen so the piano repertoire comes out recognisable. A file that
leans on a specific GM patch will sound like something else. That is the
price of a synthesiser measured in kilobytes rather than a sample set
measured in megabytes, and for Satie and Schumann it is not a high one.
