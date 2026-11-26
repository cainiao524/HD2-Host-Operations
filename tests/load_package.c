/* Load the packaged DLL through the public SDK entry point, without starting the game. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "hellforge_mod.h"

static HfTickCallback ticks[2];
static unsigned nticks;
static HfEventCallback end_session;
static HfMod *const owner = (HfMod *)(uintptr_t)1;
static void log_message(HfMod *mod, const char *format, ...) { (void)mod; (void)format; }
static const char *build_id(void) { return "25480438"; }
static int listen(HfMod *mod, HfEvent event, HfEventCallback cb, void *user)
{
    if (mod != owner || event != HF_EVENT_SESSION_END || user) return 1;
    end_session = cb;
    return 0;
}
static int on_game_thread(HfMod *mod, HfTickCallback cb, void *user)
{
    if (mod != owner || !cb || nticks >= 2 || user) return 0;
    ticks[nticks++] = cb;
    return 1;
}
static int mock_hook(HfMod *mod, uintptr_t address, void *detour, void **original)
{
    (void)mod; (void)address; (void)detour; (void)original;
    return 1;
}
int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) return 2;
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) { fprintf(stderr,"LoadLibraryW failed (%lu)\n",GetLastError()); return 1; }
    int (*init)(const HfApi *,HfMod *) = (void *)GetProcAddress(module,"hellforge_mod_init");
    if (!init) return 1;
    HfApi sdk = {.version=16, .size=sizeof sdk, .build_id=build_id, .on=listen, .on_game_thread=on_game_thread,
                 .hook=mock_hook, .log=log_message};
    if (init(&sdk,owner) || !end_session || nticks != 2 || ticks[0] == ticks[1]) return 1;
    end_session(owner,HF_EVENT_SESSION_END,NULL);
    FreeLibrary(module);
    puts("PASS: Windows loaded the released DLL, SDK 16 registered game and renderer callbacks, session closed.");
    return 0;
}
