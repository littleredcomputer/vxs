#!/bin/sh
# Generates src/vx_embedded_libs.h — every lib/*.scm as a name -> source
# table compiled into the binary.
#
# The prelude taught this lesson once already: the wasm build has no
# filesystem, so `load` cannot reach lib/*.scm, and the browser is exactly
# where a livecoding library is wanted. Embedding the prelude solved it for
# one file by evaluating it at startup. That does not generalize — a WGSL
# compiler should not cost every VM its parse time just so the browser can
# reach it.
#
# So: embed them all as DATA, evaluate none of them, and give `load` a
# fallback to the table when the file is not on disk. Nothing is parsed
# until something loads it, and (load "lib/wgsl.scm") means the same thing
# natively and in the browser.
#
# On-disk files win over embedded copies, so editing a lib and re-running
# natively picks up the edit without a rebuild.
set -e

LIBDIR="${1:-../lib}"

printf '// Generated from lib/*.scm by embed_libs.sh — DO NOT EDIT.\n'
printf '#pragma once\n\n'
printf 'namespace vxs {\n\n'
printf 'struct EmbeddedLib { const char *name; const char *source; };\n\n'
printf 'inline constexpr EmbeddedLib VX_EMBEDDED_LIBS[] = {\n'

for f in "$LIBDIR"/*.scm; do
  [ -e "$f" ] || continue
  name=$(basename "$f")
  printf '  { "%s", R"VXSCM(\n' "$name"
  cat "$f"
  printf ')VXSCM" },\n'
done

printf '};\n\n'
printf 'inline constexpr int VX_EMBEDDED_LIB_COUNT =\n'
printf '    static_cast<int>(sizeof(VX_EMBEDDED_LIBS) / sizeof(VX_EMBEDDED_LIBS[0]));\n\n'
printf '} // namespace vxs\n'
