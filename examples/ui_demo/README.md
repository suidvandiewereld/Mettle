# UI Demo

Native Windows documentation browser for Mettle. At runtime it scans the repository `docs/` tree, lists every Markdown file, and shows the selected document in a native read-only viewer with converted, wrapped text.

## Prerequisites

- Windows
- Mettle built (`bin\mettle.exe`)
- `gcc` (compiles `stdlib/dir_helpers.c` for directory scanning)

## Build

From the repository root:

```bat
examples\ui_demo\build.bat
```

This links `stdlib/dir_helpers.c` for `dir_list_md_files` and builds with the internal PE linker (`user32` + `gdi32` + CRT file I/O).

## Run

Run from the **repository root** so the app can resolve `docs/` (it looks for `docs/LANGUAGE.md`):

```bat
examples\ui_demo\ui_demo.exe
```

If launched from another working directory, the demo also tries `../docs`, `../../docs`, and `<cwd>/docs`.

## Features

| Area | What it demonstrates |
|------|----------------------|
| **Dynamic catalog** | `dir_list_md_files` recursively scans `docs/`; entries sorted alphabetically |
| **Reliable navigation** | Fixed-size path table keeps sidebar labels aligned with loaded files |
| **File loading** | `fopen` / `fread` loads Markdown on selection |
| **Readable rendering** | Markdown converted to plain text (headings, lists, code blocks, rules) in a native multiline viewer with word wrap and scroll |
| **Chrome** | Hero banner, accent themes, status bar, File menu |

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| **F5** | Rescan `docs/` and reload catalog |
| **Esc** | Exit |

The document viewer uses native scrolling (mouse wheel, scrollbar, keyboard when focused).

## Source

- [`ui_demo.mettle`](ui_demo.mettle) — UI shell, Markdown layout, file I/O
- [`stdlib/std/dir.mettle`](../../stdlib/std/dir.mettle) — `dir_list_md_files`
- [`stdlib/std/ui.mettle`](../../stdlib/std/ui.mettle) — Win32 UI layer

See [`docs/standard-library.md`](../../docs/standard-library.md) for the full `std/ui` and `std/dir` APIs.
