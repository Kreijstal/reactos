# Remove column-zero '#' directive lines from a preprocessed assembly file.
#
# cl /EP keeps any surviving '#pragma' directive in its output (e.g. the
# '#pragma once' pulled in through <sdkddkver.h>), whereas the GAS path
# treats '#' as a comment. ml/ml64 reject such a leading '#' with error
# A2044 "invalid character in file". Strip those lines before assembly.
#
# Invoked as: cmake -DIN=<in> -DOUT=<out> -P strip-asm-pragmas.cmake
#
# Operates on the whole file as a single string (not file(STRINGS), which
# would split on ';' and corrupt MASM comments). Only lines whose first
# character is '#' are removed; mid-line '#' (e.g. "mov r12, #SyscallId")
# is preserved.

file(READ "${IN}" _content)
string(REGEX REPLACE "(^|\n)#[^\n]*" "\\1" _content "${_content}")
file(WRITE "${OUT}" "${_content}")
