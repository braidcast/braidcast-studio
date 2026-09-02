#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern void install_dll_blocklist_hook(void);
extern void log_blocked_dlls(void);

#ifdef __cplusplus
}
#endif
