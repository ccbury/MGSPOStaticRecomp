# ATRAC3+ decoder core

`bitstream.cpp`, `dsp.cpp`, `atrac_common.cpp`, `atrac3plus.h`, `atrac3plus_data.h` and `atrac.h` come from
FFmpeg's libavcodec (`atrac3plus.c`, `atrac3plusdsp.c`, `atrac.c` and the matching headers), copyright
Maxim Poliakovski, Konstantin Shishkov and the FFmpeg project, licensed under the GNU LGPL version 2.1 or
(at your option) any later version. The licence text is in `COPYING.LGPL-2.1`.

Changes from upstream are limited to: include paths, FFmpeg allocator/threading/logging calls replaced by the
shims in `compat.h`/`compat.cpp`, and a couple of casts and string-literal fixes for MSVC. `compat.*` and the
`../atrac3plus.*` front end are written for this project.

Anyone redistributing a build of the runtime needs to honour the LGPL for this directory (provide the source
of these files and allow relinking).
