/*
 * PROJECT:     ReactOS ucrt library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 aliases for the rotate helpers
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 *
 * Clang treats _rotl/_rotr and friends as builtins, so the out-of-line
 * definitions in rotl.cpp/rotr.cpp are compiled under renamed symbols
 * (___rotl, ...) via the _rotl=___rotl compile definitions. The ucrtbase
 * exports use the public names, so provide tail-call aliases here, mirroring
 * the x86/x64 clang-hacks.s. The public names must be #undef'd first, as the
 * same rename definitions are applied while preprocessing this file.
 */

    .text
    .align 2

#undef _lrotl
    .global _lrotl
_lrotl:
    b ___lrotl

#undef _lrotr
    .global _lrotr
_lrotr:
    b ___lrotr

#undef _rotl
    .global _rotl
_rotl:
    b ___rotl

#undef _rotl64
    .global _rotl64
_rotl64:
    b ___rotl64

#undef _rotr
    .global _rotr
_rotr:
    b ___rotr

#undef _rotr64
    .global _rotr64
_rotr64:
    b ___rotr64
