/*
 * signal_handling.h
 * Signal handling and process control
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef _SIGNAL_HANDLING_H
#define _SIGNAL_HANDLING_H

#include "boondock_airband.h"

void sighandler(int sig);
void* controller_thread(void* params);

#endif /* _SIGNAL_HANDLING_H */
