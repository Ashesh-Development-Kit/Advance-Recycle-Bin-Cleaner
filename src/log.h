#ifndef ARBC_LOG_H
#define ARBC_LOG_H
#include <wchar.h>

void adk_log(int error, const wchar_t *message);
void adk_logf(int error, const wchar_t *format, ...);

#endif
