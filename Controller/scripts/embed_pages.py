"""Refresh the UI pages embedded by src/CMakeLists.txt on incremental builds.

`embed_binary()` in src/CMakeLists.txt reads data/*.html at CMake *configure*
time, and PlatformIO does not re-run configure when a page changes, so an
edited page would otherwise stay stale in the firmware until the build
directory was cleaned. This script runs before the SCons build starts and
rewrites the generated assembly in place; SCons tracks it as a source, so the
object is recompiled and relinked whenever the content actually changes
(rewriting identical bytes is free -- PlatformIO decides by content).

Keep PAGES in sync with the embed_binary() calls in src/CMakeLists.txt.
"""

import os

Import("env")

PROJECT_DIR = env.subst("$PROJECT_DIR")
BUILD_DIR = env.subst("$BUILD_DIR")
ASM_DIR = os.path.join(BUILD_DIR, "esp-idf", "src")

# (source page, generated assembly, symbol prefix)
PAGES = (
    ("data/index.html", "_binary_index_html.S", "_binary_index_html"),
    ("data/tally.html", "_binary_tally_html.S", "_binary_tally_html"),
    ("data/director.html", "_binary_director_html.S", "_binary_director_html"),
)


def render(path, symbol):
    with open(path, "rb") as page:
        data = page.read()

    lines = [
        "/* Generated from %s by scripts/embed_pages.py; do not edit. */" % path,
        ".section .rodata.embedded",
        ".global %s_start" % symbol,
        ".global %s_end" % symbol,
        ".balign 4",
        "%s_start:" % symbol,
    ]
    # 16 bytes per .byte line, same as src/CMakeLists.txt.
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        lines.append(".byte " + ", ".join("0x%02x" % byte for byte in chunk))
    lines.extend(["%s_end:" % symbol, ""])
    return "\n".join(lines)


def refresh():
    if not os.path.isdir(ASM_DIR):
        # A clean build lets CMake generate the assembly itself; nothing to
        # refresh before the first configure.
        return
    for page, asm_name, symbol in PAGES:
        page_path = os.path.join(PROJECT_DIR, page)
        asm_path = os.path.join(ASM_DIR, asm_name)
        try:
            text = render(page_path, symbol)
            with open(asm_path, "w") as asm:
                asm.write(text)
        except OSError as exc:
            print("embed_pages: cannot refresh %s: %s" % (page, exc))


refresh()
