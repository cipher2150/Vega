#pragma once

// 128-bit integers for intermediate products of two 1e-8 fixed-point values (~1e24).
// __extension__ keeps GCC's -Wpedantic quiet about the non-standard type (Clang accepts
// it silently); both compilers support it on every 64-bit target we build for.

namespace cb {

__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;

}  // namespace cb
