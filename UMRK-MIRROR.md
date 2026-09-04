# About this mirror

**Fun Drastic is tenlevels' work.** Everything else in this repository —
`src/`, `shared/`, `targets/`, `docs/`, the build and packaging scripts,
`LICENSE`, `CREDITS.md`, `README.md` — is his, kept verbatim. This file is the
only thing UMRK has added, and it exists solely to say where the tree came from.

tenlevels wrote the hook, designed the menus, themes and overlays, brought up
the MLP1 target, and donated the source so Leaf can build Fun DraStic from
source instead of shipping a prebuilt binary. He also wrote the `targets/leaf/`
port and the "Leaf" theme. None of that is ours.

## Provenance

| | |
| --- | --- |
| Source drop | `fun_drastic.zip` |
| SHA-256 | `c96a469c3e2bd79ad3e8175b9d150b1671bc11d323552f5ed73605d0731af0e5` |
| Received | 2026-09-04 |
| Omitted | `build/` and `dist/` only — regenerated output his own `.gitignore` excludes |

No file has been edited, reformatted, or relicensed. To re-sync a later drop,
replace the tree and commit; keep this file and keep his verbatim.

## Licensing

Fun Drastic's own code and packaging are under the **PolyForm Noncommercial
License 1.0.0** — see [LICENSE](LICENSE), which carries the required notice
`Copyright (c) 2026 The Fun Drastic authors`. Noncommercial use only. Anything
built from this tree, including Leaf releases that bundle it, inherits that
restriction.

Third-party components keep their own terms and are **not** covered by that
licence — DraStic and its free BIOS and databases are Exophase's proprietary
freeware, the fonts are OFL, stb is public domain/MIT. [CREDITS.md](CREDITS.md)
is the authoritative list; read it rather than assuming.

## Where the Leaf packaging lives

This repository is the source only. The MLP1 package Leaf ships — the launch
wrapper, the state layout, the shared-save mirror with the primary DraStic
package, the release gates — is built in
[`Fun-Drastic-standalone`](https://github.com/Utility-Muffin-Research-Kitchen/Fun-Drastic-standalone),
which cross-compiles the hook from this tree. Upstream's own `targets/leaf/`
predates that work and is kept here as he shipped it.
