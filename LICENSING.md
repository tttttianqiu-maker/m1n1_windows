# Licensing of the m1n1 binary

Most files in this tree are MIT (see `LICENSE` and each file's SPDX header).

`src/spmi.c`, `src/spmi.h`, `src/tps6598x.c` and `src/tps6598x.h` come from
upstream Asahi Linux m1n1 (commit 70757dc) and are **GPL-2.0-or-later**, as
stated in their SPDX headers. They are linked into the m1n1 binary, so the
**m1n1 binaries built from this tree (m1n1.bin, m1n1.macho, and boot.bin images
containing them) are distributed under GPL-2.0-or-later**. The GPL-2 text is in
`3rdparty_licenses/LICENSE.GPL-2`. MIT-licensed files keep their own license.
