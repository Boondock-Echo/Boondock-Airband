/*
 * web_server.h
 * Web interface server for Boondock Airband
 *
 * Copyright (c) 2026 Boondock Technologies
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#ifndef _WEB_SERVER_H
#define _WEB_SERVER_H 1

#include <pthread.h>
#include <stdint.h>

// Start the web server in a separate thread
// Returns 0 on success, -1 on error
int web_server_start(int port);

// Stop the web server
void web_server_stop(void);

// Thread function for web server
void* web_server_thread(void* params);

// Add an error message to the error log
void web_server_add_error(const char* error_msg);

// Clear all errors
void web_server_clear_errors(void);

// Set the configuration file path
void web_server_set_config_path(const char* config_path);

// Get the configuration file path
const char* web_server_get_config_path(void);

// Trigger configuration reload (returns 0 on success, -1 on error)
int web_server_trigger_reload(void);

#endif /* _WEB_SERVER_H */
