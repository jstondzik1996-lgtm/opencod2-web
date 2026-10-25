#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common_types.h"
#include "imports.h"
#include "cod2_feature_config.h"
#include "PC/qcommon/crash_handler.h"
#ifdef __EMSCRIPTEN__
#    include <emscripten/emscripten.h>
#endif

extern struct WinVars_t g_wv;
extern byte sys_packetReceived[16384];
static SysInfo sys_info;
static const dvar_t *sys_cpuGHz;
static const dvar_t *sys_sysMB;
static const dvar_t *sys_gpu;
static const dvar_t *sys_configSum;
static sysEvent_t eventQue[256];
static int eventHead;
static int eventTail;

extern void Sys_InitMainThread(void);
extern language_t Win_InitLocalization(void);
extern void Dvar_Init(void);
extern double Sys_CpuGHz(void);
extern int Sys_SystemMemoryMB(void);
extern void Sys_DetectVideoCard(int maxLen, char *outDesc);
extern Bool Sys_SupportsSSE(void);
extern const dvar_t *Dvar_RegisterBool(const char *name, Bool value, int flags);
extern const dvar_t *Dvar_RegisterFloat(const char *name, float value, float min, float max, int flags);
extern const dvar_t *Dvar_RegisterInt(const char *name, int value, int min, int max, int flags);
extern const dvar_t *Dvar_RegisterString(const char *name, const char *value, int flags);
extern void Dvar_SetBool(const dvar_t *dvar, Bool value);
extern void Dvar_SetFloat(const dvar_t *dvar, float value);
extern void Dvar_SetInt(const dvar_t *dvar, int value);
extern void Dvar_SetString(const dvar_t *dvar, const char *value);
extern void Sys_CreateConsole(HINSTANCE hInstance);
extern void Sys_CreateSplashWindow(void);
extern void Sys_ShowSplashWindow(void);
extern void Sys_DestroySplashWindow(void);
extern void Sys_ShowConsole(int visLevel, qboolean quitOnClose);
extern void Sys_DestroyConsole(void);
extern void Sys_SetErrorText(const char *text);
extern int Sys_Milliseconds(void);
extern void Sys_InitStreamThread(void);
extern void Com_Init(char *commandLine);
extern void Com_Frame(void);
extern void Sys_StateHashFrame(void);
extern void Com_Printf(const char *fmt, ...);
extern void Com_Quit_f(void);
extern void Com_ShutdownEvents(void);
extern void Conbuf_AppendText(const char *msg);
extern char *Sys_ConsoleInput(void);
extern void IN_Init(void);
extern void IN_Shutdown(void);
extern void Key_Shutdown(void);
extern void Cmd_AddCommand(const char *name, void (*function)(void));
extern void Dvar_Shutdown(void);
extern void Cmd_Shutdown(void);
extern void Con_Shutdown(void);
extern void Win_ShutdownLocalization(void);
extern void RefreshQuitOnErrorCondition(void);
extern void CL_ShutdownHunkUsers(void);
extern void CL_ShutdownRef(void);
extern void NET_Restart(void);
extern void MSG_Init(msg_t *msg, byte *data, int length);
extern qboolean NET_GetPacket(netadr_t *adr, msg_t *msg);
extern void *Z_MallocInternal(int size);
extern void Z_FreeInternal(void *ptr);
extern int SDL_HasClipboardText(void);
extern char *SDL_GetClipboardText(void);
extern void SDL_free(void *ptr);
extern void I_strncpyz(char *dest, const char *src, int destsize);
extern void Cbuf_ExecuteText(int exec_when, const char *text);
extern void WinSleep(int msec);
extern MMRESULT timeBeginPeriod(int period);
extern MMRESULT timeEndPeriod(int period);
extern const char *Win_LocalizeRef(const char *ref);
extern int MessageBoxA(void *hWnd, const char *lpText, const char *lpCaption, unsigned int uType);
extern void MacPreferences_Synchronize(void);

static void Sys_In_Restart_f(void);
static void Sys_Net_Restart_f(void);
static Bool Sys_ShouldUpdateForInfoChange(void);

static void Sys_RegisterInfoDvars(void)
{
    sys_cpuGHz = Dvar_RegisterFloat("sys_cpuGHz", 0, -3.4028235e+38f, 3.4028235e+38f, 0x1011);
    sys_sysMB = Dvar_RegisterInt("sys_sysMB", 0, (int)0x80000000, 0x7fffffff, 0x1011);
    sys_gpu = Dvar_RegisterString("sys_gpu", "", 0x1011);
    sys_configSum = Dvar_RegisterInt("sys_configSum", 0, (int)0x80000000, 0x7fffffff, 0x1011);
}

void Sys_GetInfo(SysInfo *info)
{
    memcpy(info, &sys_info, sizeof(SysInfo));
}

void Sys_ArchiveInfo(int checksum)
{
    Sys_RegisterInfoDvars();

    Dvar_SetFloat(sys_cpuGHz, (float)sys_info.cpuGHz);
    Dvar_SetInt(sys_sysMB, sys_info.sysMB);
    Dvar_SetString(sys_gpu, sys_info.gpuDescription);
    Dvar_SetInt(sys_configSum, checksum);
}

void Sys_DirectXFatalError(void)
{
#ifdef __EMSCRIPTEN__
    {
        static int n;
        extern void Com_Printf(const char *, ...);
        Com_Printf("Sys_DirectXFatalError #%d (web: not exiting)\n", ++n);
        return;
    }
#endif

    {
        extern int g_gfxV60DllActive;
        extern void Com_Printf(const char *, ...);
        if (g_gfxV60DllActive) {
            static int n;
            Com_Printf("[v60] Sys_DirectXFatalError #%d (wined3d device-loss; continuing)\n", ++n);
            if (n < 1000)
                return;
        }
    }
    const char *title = Win_LocalizeRef("WIN_DIRECTX_INIT_TITLE");
    const char *body = Win_LocalizeRef("WIN_DIRECTX_INIT_BODY");
    MessageBoxA(0, body, title, 0x10);
    exit(-1);
}

void Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    Com_Printf("Out of memory: filename '%s', line %d\n", filename, line);
    const char *title = Win_LocalizeRef("WIN_OUT_OF_MEM_TITLE");
    const char *body = Win_LocalizeRef("WIN_OUT_OF_MEM_BODY");
    MessageBoxA(0, body, title, 0x10);
    exit(-1);
}

void Sys_StartProcess(const char *exeName, qboolean doexit)
{
    Com_Printf("Sys_StartProcess: not supported on Linux (%s)\n", exeName);
}

void Sys_Error(const char *error, ...)
{
    char text[4096];
    va_list argptr;

    va_start(argptr, error);
    vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);

#ifdef __EMSCRIPTEN__
    /* Renderer cleanup can itself fail after partial initialization. Preserve
     * the original reason before attempting that cleanup. */
    fprintf(stderr, "Sys_Error: %s\n", text);
#endif
    Sys_DestroySplashWindow();
    timeEndPeriod(1);
    IN_Shutdown();
    CL_ShutdownHunkUsers();
    CL_ShutdownRef();

    Conbuf_AppendText(text);
    Conbuf_AppendText("\n");
    Sys_SetErrorText(text);
    Sys_ShowConsole(1, 1);

#ifdef __EMSCRIPTEN__
    /* Avoid infinite Com_Quit_f / exit() — that yields black canvas + unreachable. */
    emscripten_cancel_main_loop();
    /*
     * PROXY_TO_PTHREAD: UI hooks live on the browser Module. Copy to a static
     * buffer so MAIN_THREAD_ASYNC_EM_ASM can read the string after this frame.
     */
    {
        static char s_fatalText[4096];
        snprintf(s_fatalText, sizeof(s_fatalText), "%s", text);
        MAIN_THREAD_ASYNC_EM_ASM({
            var msg = UTF8ToString($0);
            var M = null;
            try { if (typeof Module !== 'undefined') M = Module; } catch (e0) {}
            if (!M || typeof M.onCod2Fatal !== 'function') {
                try { if (typeof window !== 'undefined') M = window.Module; } catch (e1) {}
            }
            if (M && typeof M.onCod2Fatal === 'function')
                M.onCod2Fatal(msg);
        }, s_fatalText);
    }
    return;
#else
    for (;;) {
        Com_Quit_f();
    }
#endif
}

void Sys_NormalExit(void)
{

}

void Sys_Print(const char *msg)
{
    Conbuf_AppendText(msg);
}

char *Sys_GetClipboardData(void)
{

    return (char *)"";
}

static void Sys_QueEventInternal(int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr)
{
    sysEvent_t *ev;
    int idx;

    idx = eventHead & 0xFF;
    ev = &eventQue[idx];

    if (eventHead - eventTail > 255) {
        Com_Printf("Sys_QueEvent: overflow\n");
        if (ev->evPtr) {
            Z_FreeInternal(ev->evPtr);
        }
        eventTail++;
    }

    eventHead++;

    if (time == 0) {
        time = Sys_Milliseconds();
    }

    ev->evTime = time;
    ev->evType = type;
    ev->evValue = value;
    ev->evValue2 = value2;
    ev->evPtrLength = ptrLength;
    ev->evPtr = ptr;
}

void Sys_QueEvent(int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr)
{
    Sys_QueEventInternal(time, type, value, value2, ptrLength, ptr);
}

static void Sys_In_Restart_f(void)
{
    IN_Shutdown();
    IN_Init();
}

static void Sys_Net_Restart_f(void)
{
    NET_Restart();
}

void Sys_Init(void)
{
    timeBeginPeriod(1);

    Cmd_AddCommand("in_restart", Sys_In_Restart_f);
    Cmd_AddCommand("net_restart", Sys_Net_Restart_f);

    Com_Printf("Measured CPU speed is %.2lf GHz\n", sys_info.cpuGHz);
    Com_Printf("System memory is %i MB (capped at 1 GB)\n", sys_info.sysMB);
    Com_Printf("Video card is \"%s\"\n", sys_info.gpuDescription);
    Com_Printf("Streaming SIMD Extensions (SSE) %ssupported\n", sys_info.SSE ? "" : "not ");
    Com_Printf("\n");
#ifdef __EMSCRIPTEN__
    Com_Printf("webdbg: before IN_Init\n");
#endif
    IN_Init();
#ifdef __EMSCRIPTEN__
    Com_Printf("webdbg: after IN_Init\n");
#endif
}

void Sys_LoadingKeepAlive(void)
{

    UpdateSystemActivity(0);
}

static HINSTANCE g_hInstance;

#ifdef __EMSCRIPTEN__
static void Sys_WebFrame(void)
{
#ifdef __EMSCRIPTEN__
    {
        static int s_webFrames;
        if (s_webFrames < 3) {
            Com_Printf("webdbg: Sys_WebFrame #%d\n", s_webFrames);
            s_webFrames++;
        }
    }
#endif
    Com_Frame();
}
#endif

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    char cwd[256];

#ifdef _WIN32

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

#ifndef __EMSCRIPTEN__

    Sys_InstallCrashHandler("CoD2 MP " COD2_VERSION_SHORT, COD2_VERSION_SHORT,
                            COD2_VERSION_DATE, lpCmdLine);
#endif

    Sys_InitMainThread();
    Win_InitLocalization();
    Dvar_Init();

    {
        extern int sv_disableClientConsole;
        if (!sv_disableClientConsole)
            sv_disableClientConsole = (int)Dvar_RegisterBool("sv_disableClientConsole", 0, 0x1008);
    }

    sys_info.cpuGHz = Sys_CpuGHz();
    sys_info.sysMB = Sys_SystemMemoryMB();
    Sys_DetectVideoCard(0x200, sys_info.gpuDescription);
    sys_info.SSE = Sys_SupportsSSE();

    {
        const dvar_t *sys_SSE = Dvar_RegisterBool("sys_SSE", 0, 0);
        Dvar_SetBool(sys_SSE, sys_info.SSE);
    }

    g_hInstance = hInstance;
    Sys_CreateConsole(hInstance);
#ifndef DEDICATED
    Sys_CreateSplashWindow();
    Sys_ShowSplashWindow();
#endif
    Sys_Milliseconds();
    Sys_InitStreamThread();

#ifdef DEDICATED

    {
        static char ded_cmdline[2048];
        snprintf(ded_cmdline, sizeof(ded_cmdline), "+set dedicated 2 %s",
                 lpCmdLine ? lpCmdLine : "");
        Com_Init(ded_cmdline);
    }
#else
    Com_Init(lpCmdLine);
#endif

    getcwd(cwd, sizeof(cwd));
    Com_Printf("Working directory: %s\n", cwd);

#ifdef __EMSCRIPTEN__
    Com_Printf("webdbg: entering emscripten main loop\n");
    /*
     * Com_Init has returned: GL context + CL_Init are done. Notify the shell
     * on the UI thread (ASYNC — do not deadlock the worker during boot).
     */
    MAIN_THREAD_ASYNC_EM_ASM({
        try {
            var M = null;
            try { if (typeof Module !== 'undefined') M = Module; } catch (e0) {}
            if (!M || typeof M.onCod2Ready !== 'function') {
                try { if (typeof window !== 'undefined') M = window.Module; } catch (e1) {}
            }
            if (M && typeof M.onCod2Ready === 'function')
                M.onCod2Ready();
        } catch (e) {}
    });
    emscripten_set_main_loop(Sys_WebFrame, 0, 1);
    return 0;
#else

    for (;;) {
        Com_Frame();
        Sys_StateHashFrame();
    }

    return 0;
#endif
}

void Sys_Quit(void)
{
    int i;
    sysEvent_t *ev;

    timeEndPeriod(1);
    IN_Shutdown();
    Key_Shutdown();
    Sys_DestroyConsole();
    Win_ShutdownLocalization();
    RefreshQuitOnErrorCondition();
    Dvar_Shutdown();
    Cmd_Shutdown();
    Con_Shutdown();
    Com_ShutdownEvents();

    while (eventHead > eventTail) {
        int idx = eventTail & 0xFF;
        eventTail++;
        ev = &eventQue[idx];
        if (ev->evPtr) {
            Z_FreeInternal(ev->evPtr);
        }
    }

    MacPreferences_Synchronize();
#ifdef __EMSCRIPTEN__
    fprintf(stderr, "Sys_Quit\n");
    emscripten_cancel_main_loop();
    MAIN_THREAD_ASYNC_EM_ASM({
        var M = null;
        try { if (typeof Module !== 'undefined') M = Module; } catch (e0) {}
        if (!M || typeof M.onCod2Quit !== 'function') {
            try { if (typeof window !== 'undefined') M = window.Module; } catch (e1) {}
        }
        if (M && typeof M.onCod2Quit === 'function')
            M.onCod2Quit();
    });
#else
    exit(0);
#endif
}

sysEvent_t Sys_GetEvent(void)
{
    sysEvent_t ev;
    char *s;
    int len;
    msg_t netmsg;
    netadr_t adr;

    extern void IN_Frame(void);
    IN_Frame();

    if (eventTail < eventHead) {
        int idx = eventTail & 0xFF;
        eventTail++;
        return eventQue[idx];
    }

    s = Sys_ConsoleInput();
    if (s) {
        len = strlen(s);
        char *b = (char *)Z_MallocInternal(len + 1);
        I_strncpyz(b, s, len + 1);
        Sys_QueEventInternal(0, (sysEventType_t)4, 0, 0, len + 1, b);
    }

    MSG_Init(&netmsg, sys_packetReceived, 0x4000);
    if (NET_GetPacket(&adr, &netmsg)) {
        len = netmsg.cursize - netmsg.readcount + 12;
        byte *buf = (byte *)Z_MallocInternal(len);
        memcpy(buf, &adr, 12);
        memcpy(buf + 12, netmsg.data + netmsg.readcount, netmsg.cursize - netmsg.readcount);
        Sys_QueEventInternal(0, (sysEventType_t)5, 0, 0, len, buf);
    }

    if (eventTail < eventHead) {
        int idx = eventTail & 0xFF;
        eventTail++;
        return eventQue[idx];
    }

    memset(&ev, 0, sizeof(ev));
    ev.evTime = Sys_Milliseconds();
    return ev;
}

static Bool Sys_ShouldUpdateForInfoChange(void)
{
    Sys_RegisterInfoDvars();
    Sys_ArchiveInfo(0);

    const char *title = Win_LocalizeRef("WIN_COMPUTER_CHANGE_TITLE");
    const char *body = Win_LocalizeRef("WIN_COMPUTER_CHANGE_BODY");
    return MessageBoxA(0, body, title, 0x44) == 6;
}

Bool Sys_HasConfigureChecksumChanged(int checksum)
{
    Bool result;

    Sys_RegisterInfoDvars();

    if (sys_configSum->current.integer != 0 && sys_configSum->current.integer != checksum) {
        const char *title = Win_LocalizeRef("WIN_CONFIGURE_UPDATED_TITLE");
        const char *body = Win_LocalizeRef("WIN_CONFIGURE_UPDATED_BODY");
        result = (MessageBoxA(0, body, title, 0x44) == 6);
    } else {
        result = 0;
    }

    if (sys_configSum->current.integer == 0 || sys_configSum->current.integer != checksum) {
        Dvar_SetInt(sys_configSum, checksum);
    }

    return result;
}

Bool Sys_HasInfoChanged(void)
{
    double cpuCurrent;
    int sysMBCurrent;

    Sys_RegisterInfoDvars();

    cpuCurrent = (double)sys_cpuGHz->current.value;

    if (cpuCurrent > sys_info.cpuGHz * 1.1 || cpuCurrent < sys_info.cpuGHz * 0.9) {
        return Sys_ShouldUpdateForInfoChange();
    }

    sysMBCurrent = sys_sysMB->current.integer;
    if (sysMBCurrent > sys_info.sysMB + 32 || sysMBCurrent < sys_info.sysMB - 32) {
        return Sys_ShouldUpdateForInfoChange();
    }

    if (strcmp((const char *)sys_gpu->current.string, sys_info.gpuDescription) != 0) {
        return Sys_ShouldUpdateForInfoChange();
    }

    return 0;
}
