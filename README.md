# ForgeFM

ForgeFM is a Linux-oriented file manager prototype written from scratch in C with GTK4/GIO.

## MVP 0.2

- Back / Forward / Up / Reload navigation
- Path entry with `Ctrl+L`
- Sidebar: Home, common user folders, Filesystem
- List view with file type, size and modified time
- Multiple selection
- Hidden files toggle
- Search within the current directory (`Ctrl+F`)
- Create empty files and folders
- Rename (`F2`)
- Move to Trash (`Delete`)
- Internal Copy / Cut / Paste (`Ctrl+C`, `Ctrl+X`, `Ctrl+V`)
- Automatic conflict-safe destination names (`file (1).txt`)
- Open files with the desktop default application
- Open a terminal in the current directory
- Keyboard-first workflow

## Build on NixOS

```bash
unzip forgefm-mvp.zip
cd forgefm-mvp
nix develop
meson setup build
meson compile -C build
./build/forgefm
```

## Notes

This is intentionally an MVP, not a Dolphin replacement. The next development stages can add tabs, split view, asynchronous copy/move progress, previews, permissions, bookmarks, mounts/devices, archives, a command palette, and a plugin system.

ForgeFM is implemented independently. Existing file managers may be studied for architecture and UX ideas, but their source is not copied into this project.
