"""compile_po.py  <input.po>  <output.mo>
Compiles a .po file to a binary .mo file using the polib library.
Used by CMake as a replacement for msgfmt when gettext is not installed.
"""
import sys
import polib

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <input.po> <output.mo>", file=sys.stderr)
    sys.exit(1)

po = polib.pofile(sys.argv[1], encoding="utf-8")
po.save_as_mofile(sys.argv[2])
print(f"Compiled: {sys.argv[1]} -> {sys.argv[2]}")
