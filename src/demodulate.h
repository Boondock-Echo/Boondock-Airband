/*
 * demodulate.h
 * Main demodulation thread
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef _DEMODULATE_H
#define _DEMODULATE_H

#include "boondock_airband.h"

void* demodulate(void* params);

#endif /* _DEMODULATE_H */
