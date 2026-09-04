# Credits

Fun Drastic — name and concept: **tenlevels**.

Fun Drastic is a frontend and compatibility layer; the work below is other
people's, and it is what makes Fun Drastic possible.

## The emulator

- **DraStic** — Nintendo DS emulator by **Exophase** (with Lordus and slaanesh).
  Proprietary freeware; all emulation is DraStic's. Fun Drastic hooks the
  publicly distributed binaries unmodified and is **not affiliated with or
  endorsed by** Exophase.
- **DraStic free BIOS** — the drop-in ARM7/ARM9 BIOS replacement bundled with
  DraStic, used so no Nintendo BIOS files are required.

## Libraries

- **SDL2** — Simple DirectMedia Layer (zlib license). Fun Drastic targets the
  SDL2 API; DraStic's own SDL2 provides it at run time.
- **stb_image** and **stb_truetype** — image loading and TrueType rasterising
  by Sean Barrett (public domain / MIT).

## Fonts

- **Noto Sans CJK SC** (shipped as `fonts/Translate.otf`) — Google, under the
  SIL Open Font License 1.1. Renders translated menus, including Chinese. The
  default menu uses Fun Drastic's own built-in bitmap font; this font is used
  only for languages the bitmap font can't draw.

## Data

- **usrcheat.dat** — community-maintained DS cheat database, assembled over
  many years by the DS cheat scene.
- **game_database.xml** — DraStic's save-type database.

## Per-device packages

A CFW/device package may bundle its own extras — a theme font, an SDL stack,
libraries specific to that firmware. Those carry their own credits in their
target folder, not here.

## Trademarks

Nintendo, Nintendo DS, and all game titles are trademarks of their respective
owners. Fun Drastic contains no Nintendo code, art, or assets; theme names
are color-palette names only.
