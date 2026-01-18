/*
 * capture_process.h
 * Capture subprocess management
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef _CAPTURE_PROCESS_H
#define _CAPTURE_PROCESS_H

#include <sys/types.h>

// Capture process management
pid_t capture_process_start(const char* config_path);
int capture_process_stop(void);
int capture_process_is_running(void);
pid_t capture_process_get_pid(void);
int capture_process_wait(void);

// IPC communication
int capture_process_send_command(const char* command);
int capture_process_get_status(char* status_buffer, size_t buffer_size);

// Status file management
#define CAPTURE_STATUS_FILE "/tmp/boondock_airband_capture.status"
#define CAPTURE_PID_FILE "/tmp/boondock_airband_capture.pid"
#define CAPTURE_CMD_PIPE "/tmp/boondock_airband_capture.cmd"

#endif /* _CAPTURE_PROCESS_H */
