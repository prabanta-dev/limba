/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * luxia.h - the limba program on a Luxia source.
 */
#ifndef LIMBA_TOOL_LUXIA_H
#define LIMBA_TOOL_LUXIA_H

#include <stdbool.h>

/* compile in as far as the front end goes; emit may be NULL; the exit
   status of the program */
int limba_luxia_main(const char *in, const char *emit, const char *outpath,
                     bool check);

#endif
