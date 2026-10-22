/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * endian.h - for the mathematics of musl only, on a C library that has
 * none (newlib, on a microcontroller): the byte order musl's libm.h asks,
 * from the compiler's own macros. build.sh gives this directory with
 * -idirafter, so the C library's endian.h comes first where it exists.
 */
#ifndef LIMBA_MUSL_ENDIAN_H
#define LIMBA_MUSL_ENDIAN_H

#define __LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#define __BIG_ENDIAN __ORDER_BIG_ENDIAN__
#define __BYTE_ORDER __BYTE_ORDER__

#endif
