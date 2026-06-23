# amc - alxmra's macros
### _X11 click-macro builder_

Minimal C++ tool. Drop numbered crosshair markers at the mouse position (color = mouse button), insert keyboard actions, then replay the whole ordered sequence with `xdotool`. Overlay via `osd_cat`, keyboard-payload entry via `dmenu`.

Works across monitors automatically: it captures global pointer coords, and converts them to `osd_cat`'s primary-monitor-relative offsets via XRandR.

## Build

```sh
make
```

Requires: X11 + Xrandr dev libs (link-time), and at runtime: `osd_cat` (xosd), `xdotool`, `dmenu`.

## Run

```sh
./macros [-d ms] [-r n] [-h] [file.amc]
```

| Arg | Meaning | Default |
|------|---------|---------|
| `file.amc` | load this macro on startup; markers appear at saved spots | |
| `-d ms` | delay between actions on replay | 150 |
| `-r n`  | Shift+F9 default repeat; `0` = loop until F11 | 1 |
| `-h`    | help | |

## Hotkeys (global)

| Key | Action | Color |
|-----|--------|-------|
| F1  | add LEFT-click marker at cursor   | green |
| F2  | add RIGHT-click marker at cursor  | red |
| F3  | add MIDDLE-click marker at cursor | cyan |
| F4  | add keyboard action | yellow |
| F5  | undo last action | |
| F6  | add WAIT action | white |
| F7  | save sequence to `.amc` | |
| F8  | load `.amc` | |
| F9  | replay once | |
| Shift+F9 | replay N times | |
| F10 | clear all | |
| F11 | cancel a running loop replay | |
| F12 | quit | |

### Keyboard actions (F4)

A `dmenu` prompt appears. Type:

- `key:<combo>` to send a key combo, e.g. `key:ctrl+shift+t`, `key:Return`, `key:alt+F4`
- anything else is typed literally as text

Combos use `xdotool key` syntax; text uses `xdotool type`.

### Wait (F6)

Inserts a pause into the sequence. `dmenu` prompts for milliseconds (this is in addition to the global `-d` delay between every action). Marker shows `W N`.

### Replay (F9 / Shift+F9)

- F9 replays once, no prompt (ignores `-r`).
- Shift+F9 prompts for a count:
  - blank or Esc uses the `-r` default
  - a number `n` replays `n` times
  - `0` loops until F11 cancels

## Saving / loading (`.amc`)

- F7 saves the current sequence to a file (a `.amc` extension is added if missing).
- F8 loads a file at runtime (replaces the current sequence, redraws markers).
- Passing a file as a CLI arg loads it on startup, so markers appear immediately at their saved positions:

```sh
./macros mymacro.amc
```

Format is plain text, one action per line, `TYPE x y payload`; `#` lines and blank lines ignored. Example:

```
# amc v1
LCLICK 1070 540
WAIT 0 0 500
RCLICK 200 300
KEYCOMBO 1070 540 ctrl+s
TYPE 400 400 hello world
```

`x y` are global screen coords. Hand-editable.

## Notes

- Markers persist until undo (F5), clear (F10), or quit (F12); on quit all `osd_cat` overlay processes are killed.
- Each marker shows `+ N` (clicks) or `K N` (keyboard), N = order in the sequence.
- The active display is wherever the mouse is; coords are global, so no config needed.
