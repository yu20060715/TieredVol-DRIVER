#ifndef EXEC_HELPER_H
#define EXEC_HELPER_H

#include <stddef.h>

int tv_exec_sudo(char *const argv[], int quiet);
int tv_exec_capture(const char *path, char *const argv[], char *out, size_t outsize);
int tv_exec_with_stdin(const char *path, char *const argv[], const char *stdin_data);

#endif
