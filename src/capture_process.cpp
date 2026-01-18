/*
 * capture_process.cpp
 * Capture subprocess management
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "capture_process.h"
#include "logging.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

static pid_t capture_pid = 0;

pid_t capture_process_get_pid(void) {
    // Try to read PID from file
    FILE* f = fopen(CAPTURE_PID_FILE, "r");
    if (f) {
        pid_t pid = 0;
        if (fscanf(f, "%d", &pid) == 1) {
            // Verify process is still running
            if (kill(pid, 0) == 0) {
                capture_pid = pid;
                fclose(f);
                return pid;
            }
        }
        fclose(f);
    }
    return 0;
}

int capture_process_is_running(void) {
    pid_t pid = capture_process_get_pid();
    if (pid == 0) {
        return 0;
    }
    // Check if process is still alive
    if (kill(pid, 0) == 0) {
        return 1;
    }
    // Process is dead, clean up
    capture_pid = 0;
    unlink(CAPTURE_PID_FILE);
    unlink(CAPTURE_STATUS_FILE);
    return 0;
}

pid_t capture_process_start(const char* config_path) {
    // Check if already running
    if (capture_process_is_running()) {
        log(LOG_WARNING, "Capture process is already running (PID: %d)\n", capture_process_get_pid());
        return capture_process_get_pid();
    }

    // Create command pipe for IPC
    unlink(CAPTURE_CMD_PIPE);
    if (mkfifo(CAPTURE_CMD_PIPE, 0666) != 0 && errno != EEXIST) {
        log(LOG_ERR, "Failed to create command pipe: %s\n", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log(LOG_ERR, "Failed to fork capture process: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child process: exec capture subprocess
        char* argv[4];
        char prog_name[] = "boondock_airband";
        char capture_flag[] = "--capture";
        char config_arg[1024];
        strncpy(config_arg, config_path, sizeof(config_arg) - 1);
        config_arg[sizeof(config_arg) - 1] = '\0';
        argv[0] = prog_name;
        argv[1] = capture_flag;
        argv[2] = config_arg;
        argv[3] = NULL;

        // Redirect stdout/stderr to log file or /dev/null
        int logfd = open("/tmp/boondock_airband_capture.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            if (logfd > 2) close(logfd);
        }

        execvp(argv[0], argv);
        // If exec fails
        log(LOG_CRIT, "Failed to exec capture process: %s\n", strerror(errno));
        _exit(1);
    }

    // Parent process: save PID
    capture_pid = pid;
    FILE* f = fopen(CAPTURE_PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", (int)pid);
        fclose(f);
    }

    // Wait a moment to see if process starts successfully
    usleep(500000); // 500ms
    if (kill(pid, 0) == 0) {
        log(LOG_INFO, "Capture process started (PID: %d)\n", pid);
        return pid;
    } else {
        log(LOG_ERR, "Capture process failed to start\n");
        capture_pid = 0;
        unlink(CAPTURE_PID_FILE);
        return -1;
    }
}

int capture_process_stop(void) {
    pid_t pid = capture_process_get_pid();
    if (pid == 0) {
        log(LOG_WARNING, "Capture process is not running\n");
        return 0;
    }

    log(LOG_INFO, "Stopping capture process (PID: %d)\n", pid);

    // Send SIGTERM for graceful shutdown
    if (kill(pid, SIGTERM) != 0) {
        log(LOG_ERR, "Failed to send SIGTERM to capture process: %s\n", strerror(errno));
        return -1;
    }

    // Wait up to 5 seconds for graceful shutdown
    for (int i = 0; i < 50; i++) {
        usleep(100000); // 100ms
        if (kill(pid, 0) != 0) {
            // Process has exited
            capture_pid = 0;
            unlink(CAPTURE_PID_FILE);
            unlink(CAPTURE_STATUS_FILE);
            log(LOG_INFO, "Capture process stopped gracefully\n");
            return 0;
        }
    }

    // Force kill if still running
    log(LOG_WARNING, "Capture process did not stop gracefully, sending SIGKILL\n");
    if (kill(pid, SIGKILL) != 0) {
        log(LOG_ERR, "Failed to send SIGKILL to capture process: %s\n", strerror(errno));
        return -1;
    }

    // Wait for process to die
    waitpid(pid, NULL, 0);
    capture_pid = 0;
    unlink(CAPTURE_PID_FILE);
    unlink(CAPTURE_STATUS_FILE);
    log(LOG_INFO, "Capture process force-stopped\n");
    return 0;
}

int capture_process_wait(void) {
    pid_t pid = capture_process_get_pid();
    if (pid == 0) {
        return 0;
    }
    int status;
    if (waitpid(pid, &status, 0) == pid) {
        capture_pid = 0;
        unlink(CAPTURE_PID_FILE);
        unlink(CAPTURE_STATUS_FILE);
        return status;
    }
    return -1;
}

int capture_process_send_command(const char* command) {
    if (!capture_process_is_running()) {
        return -1;
    }

    int fd = open(CAPTURE_CMD_PIPE, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    size_t len = strlen(command);
    ssize_t written = write(fd, command, len);
    close(fd);
    return (written == (ssize_t)len) ? 0 : -1;
}

int capture_process_get_status(char* status_buffer, size_t buffer_size) {
    FILE* f = fopen(CAPTURE_STATUS_FILE, "r");
    if (!f) {
        return -1;
    }

    if (fgets(status_buffer, buffer_size, f)) {
        fclose(f);
        // Remove newline
        size_t len = strlen(status_buffer);
        if (len > 0 && status_buffer[len - 1] == '\n') {
            status_buffer[len - 1] = '\0';
        }
        return 0;
    }

    fclose(f);
    return -1;
}
