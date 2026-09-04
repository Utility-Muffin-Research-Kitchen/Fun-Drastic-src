# Overlay templates

Each `<width>x<height>/Template/` holds the neutral overlay set for a panel of
that resolution: a dark fill with DraStic's screen area(s) cut out transparent,
one PNG per layout and scaling mode. They are starting points — a theme author
paints a bezel in the opaque area; the transparent window is where the emulator
renders.

Filenames are `<mode>_<layout>.png`:

- mode — `aspect` (fit to panel) or `pp` (pixel-perfect integer scale)
- layout — `single`, `stacked`, `sidebyside`, `focus`, `pip`

`single` and `pip` templates are intentionally blank (fully transparent).

The hook loads the set matching the device's panel from `Overlays/<w>x<h>/`.

## Adding a resolution

`generate.py` produces the template set for any panel size, with the screen
rectangles taken straight from the hook's own layout math, so they line up with
where each screen is drawn:

```
python3 generate.py 1280x960        # one size
python3 generate.py                 # regenerate the common set
```
