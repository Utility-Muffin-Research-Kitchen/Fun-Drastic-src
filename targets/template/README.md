# targets/template — adding a CFW / device

Copy this folder to `targets/<yourdevice>/` and fill in what your firmware
needs. `targets/leaf/` is the complete worked example. Most devices need only
a subset of the files below — delete what you don't use.

- `config/drastic.cfg` — the DraStic button map for your device. Must be a
  **full** cfg (a partial one breaks input mapping and turns on the fps
  counter). The one here is a generic starting point.
- `theme/custom.cfg.example` — copy to `theme/custom.cfg` and edit the colours
  to add a branded "CUSTOM" theme entry (optional).
- `launch.sh` — only if you must override the shared default launcher
  (`shared/launch.sh`). See `targets/leaf/launch.sh` for a full example.

You will also need a platform header at `src/platforms/platform_<yourdevice>.h`
(copy `src/platforms/platform_template.h`) and a build rule in the `Makefile`.
See [docs/BUILDING.md](../../docs/BUILDING.md) and
[CONTRIBUTING.md](../../CONTRIBUTING.md).

Anything the whole fleet shares — fonts, languages, cursor, mic sound, overlay
templates — already lives in `shared/` and is packaged automatically. Don't
copy it in here.
