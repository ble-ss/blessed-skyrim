#pragma once

// stand-in for the "common" library that skse64's own build force-includes
// (skse64/sheets/ForcedInclude.props -> common/IPrefix.h) but that isn't
// vendored in this tree. only the pieces the skse64 headers we actually use
// (GameTypes.h/GameEvents.h/GameForms.h/GameMenus.h and friends) need at
// parse time: fixed-width type aliases and a couple of logging macros used
// by inline methods we never call. this file carries no offsets or
// version-specific data, so it's not part of the 1.7.104 compat surface.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cassert>
#include <string>
#include <algorithm>
#include <new>

typedef std::uint8_t   UInt8;
typedef std::uint16_t  UInt16;
// 32-bit types match Win32's LONG/DWORD (long / unsigned long), not int /
// unsigned int - the skse64 headers pass these straight into _Interlocked*
// intrinsics overloaded on `volatile long*`, which int* isn't related to.
typedef unsigned long  UInt32;
typedef std::uint64_t  UInt64;
typedef std::int8_t    SInt8;
typedef std::int16_t   SInt16;
typedef long           SInt32;
typedef std::int64_t   SInt64;
typedef float          Float32;
typedef double         Float64;

#define STATIC_ASSERT(x) static_assert(x, #x)
#define ASSERT(x) assert(x)

// skybench does its own logging (see Log()/LogEvent() in skybench.cpp).
// these exist only so inline methods carried over from skse64 headers
// (never called by skybench) compile and link.
void skybench_DebugMessage(const char * fmt, ...);
#define _MESSAGE(...) skybench_DebugMessage(__VA_ARGS__)
#define _ERROR(...) skybench_DebugMessage(__VA_ARGS__)
#define _WARNING(...) skybench_DebugMessage(__VA_ARGS__)
#define _DMESSAGE(...) skybench_DebugMessage(__VA_ARGS__)
