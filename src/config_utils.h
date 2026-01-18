/*
 * config_utils.h
 * Configuration file utilities
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef _CONFIG_UTILS_H
#define _CONFIG_UTILS_H

#include <iostream>

void usage();
bool create_default_config(const char* config_path);
int capture_main(int argc, char* argv[]);
int web_server_main(int argc, char* argv[]);

#endif /* _CONFIG_UTILS_H */
