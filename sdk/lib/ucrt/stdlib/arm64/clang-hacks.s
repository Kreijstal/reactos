/*
 * PROJECT:     ReactOS UCRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Public rotate aliases for Clang ARM64 builds
 */

#undef _lrotl
#undef _lrotr
#undef _rotl
#undef _rotl64
#undef _rotr
#undef _rotr64

    .text

    .global _lrotl
    .global _lrotr
    .global _rotl
    .global _rotl64
    .global _rotr
    .global _rotr64

_lrotl:
    b ___lrotl

_lrotr:
    b ___lrotr

_rotl:
    b ___rotl

_rotl64:
    b ___rotl64

_rotr:
    b ___rotr

_rotr64:
    b ___rotr64
