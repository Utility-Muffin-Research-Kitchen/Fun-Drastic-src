# About this mirror

**Fun Drastic is tenlevels' work.** `src/`, `shared/`, `targets/`, `docs/`, the
build and packaging scripts, `LICENSE`, `CREDITS.md` and `README.md` are his.
This file is UMRK's, and it exists to say where the tree came from and what has
been changed on top of it.

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

The import commit is his tree exactly as sent — nothing reformatted, nothing
relicensed. To re-sync a later drop, replace the tree and commit.

## Local changes

tenlevels agreed that fixes can go straight into this tree rather than being
carried as patches downstream, so that anyone forking it gets them. Every
commit after the import is therefore a change on top of his source, and the
history is the record of what is his and what is not:

```sh
git log --oneline <import commit>..HEAD -- src shared targets docs
```

Fixes made here should go back to tenlevels so his own tree carries them too.

| Commit | Change |
| --- | --- |
| `c2e270f` | Clip cheat names to the row they are drawn in — long entries from `usrcheat.dat` ran through the `< ON/OFF >` control and off the panel |

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
