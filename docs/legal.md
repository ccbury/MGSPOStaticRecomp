# Legal model

This repository contains tooling only. It must never contain:

- the game's executable, modules, or any machine code or assembly derived from them
- game assets (models, textures, audio, video, text, scripts)
- disc images, extracted files, or dumps of any of the above
- console firmware, keys or other proprietary material

## How it works instead

- You supply a disc image of a copy of the game that you own.
- Extraction, analysis and (later) recompilation all run on your machine, from your image.
- Generated output (lifted sources, extracted data, build artifacts) stays in local, git-ignored
  directories. The `.gitignore` already excludes `*.iso`, `*.cso`, `*.elf`, `*.prx`, `EBOOT.BIN`,
  `BOOT.BIN`, `PARAM.SFO`, and the `extracted/`, `assets/` and `work/` folders.
- Documentation may describe file formats and record facts such as section sizes, entry points and
  imported library names/NIDs, but should not reproduce game code or content.

## Contributing

Before sending a change, check `git status` for anything that came out of a disc image. If a file
exists only because of the game, it does not belong in a commit.
