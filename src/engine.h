#ifndef ADK_ENGINE_H
#define ADK_ENGINE_H
#include <stdint.h>
#include <wchar.h>

enum { ADK_SCAN, ADK_KEEP, ADK_REMOVE };
typedef struct {
    uint64_t bytes, items, free_before, free_after;
    uint32_t failures, first_error;
    uint32_t free_before_known, free_after_known;
} DriveResult;

typedef void (*Progress)(void *, unsigned, unsigned, unsigned, const DriveResult *);
uint32_t adk_drives(void);
int adk_is_elevated(void);
uint32_t adk_request_desktop_admin(uint32_t, int);
void adk_run(uint32_t, unsigned, Progress, void *);
DriveResult adk_tree(void *parent, const wchar_t *name, unsigned mode);
int adk_desktop(uint32_t, int, const int32_t *);
uint32_t adk_startup_value(void *, int);
void adk_notify_quick(int);
int adk_save(const int32_t *);

#endif
