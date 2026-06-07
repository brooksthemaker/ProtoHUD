# ROMs

Drop your libretro-compatible ROM files in this folder. ProtoHUD's
**Games → Source → Emulator (libretro) → ROM** menu lists every file here — pick
one to load it (and Emulator becomes the active source). Added files after
launch? Use **ROM → Rescan Folder**.

Notes:

- This is the default ROM folder (`game.libretro_rom_dir`). Point that key in
  `config.json` somewhere else if your ROMs live elsewhere.
- You still need a libretro **core** (`game.libretro_core`, e.g.
  `snes9x_libretro.so`). See `docs/games.md` and `scripts/install-emulator.sh`.
- ROMs are user-supplied and **not** bundled or committed — this folder is
  git-ignored apart from this README and `.gitignore`.
