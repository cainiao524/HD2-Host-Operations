#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <new>

#include "overlay.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

static_assert(IMGUI_VERSION_NUM == 19180, "Build all ImGui sources and backends from v1.91.8");
static_assert(std::atomic<bool>::is_always_lock_free, "SESSION_END uses only lock-free flags");
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern "C" void host_ui_draw(bool *open);
extern "C" void host_ui_closed(void);

namespace {
using Microsoft::WRL::ComPtr;
const HfApi *hf;
HfMod *owner;
std::atomic<bool> stopped{}, visible{}, ready{};
std::atomic<HWND> inputWindow{};
SRWLOCK graphicsLock = SRWLOCK_INIT, inputLock = SRWLOCK_INIT;
struct Lock {
    SRWLOCK *p;
    explicit Lock(SRWLOCK &lock) : p(&lock) { AcquireSRWLockExclusive(p); }
    ~Lock() { ReleaseSRWLockExclusive(p); }
    Lock(const Lock &) = delete;
};

using Present = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using Present1 = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
using Resize = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1 = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                            const UINT *, IUnknown *const *);
using Create = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
using CreateHwnd = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
using Execute = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
using Barrier = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
using Reset = HRESULT (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);

// HellForge rewrites these storage locations when another mod joins a chain.
Present nextPresent;
Present1 nextPresent1;
Resize nextResize;
Resize1 nextResize1;
Create nextCreate;
CreateHwnd nextCreateHwnd;
Execute nextExecute;
Barrier nextBarrier;
Reset nextReset;
decltype(&ClipCursor) nextClip;
decltype(&SetCursorPos) nextWarp;
decltype(&GetRawInputBuffer) nextRaw;

const GUID queueKey = {0xe59c4334,0xf06d,0x4222,{0x9b,0x1c,0x8b,0xdb,0xbe,0x86,0xce,0x81}};
const GUID chainKey = {0xc6b05f53,0x951c,0x479a,{0xa8,0x1f,0x20,0x96,0xc5,0x85,0x22,0x81}};
const GUID bufferKey = {0x9582362d,0x7666,0x40ed,{0x87,0x6c,0x79,0xe8,0x1b,0x6f,0x59,0xf7}};
const GUID listKey = {0x38a7f143,0xbbd8,0x4533,{0x9e,0x1d,0x00,0xd8,0x81,0xff,0x70,0xa1}};
template<class Object, class T> bool readTag(Object *obj, const GUID &key, T &value)
{
    UINT size = sizeof(value);
    return SUCCEEDED(obj->GetPrivateData(key, &size, &value)) && size == sizeof(value);
}

void closeMenu()
{
    if (!stopped.load() && visible.exchange(false)) host_ui_closed();
}

bool capturing()
{
    HWND w = inputWindow.load();
    return w && !stopped.load() && visible.load() && GetForegroundWindow() == w;
}

struct InputEvent { UINT msg; WPARAM w; LPARAM l; };
InputEvent events[512];
unsigned eventCount;
bool lostEvents, haveClip, clipNull;
std::atomic<bool> rawMouseOnly{}, rawKeyboardOnly{};
RECT savedClip;
// Diagnostics for the cursor while the menu holds input (logged on open, every second, and on close).
std::atomic<unsigned> swallowedWarps{}, swallowedClips{}, eatenRaw{};
// The menu's pointer. The game recentres the system cursor every frame while it is in mouse-look; holding those
// calls back made the game leave mouse-look for good (the view stopped turning after the menu closed). So the
// game's SetCursorPos/ClipCursor always pass, and the menu moves its own pointer by the raw mouse deltas it eats.
std::atomic<long> virtualX{}, virtualY{};
// Raw input the game gets while the menu is closed (WM_INPUT messages and GetRawInputBuffer packets).
std::atomic<unsigned> gameRawMessages{}, gameRawBuffered{};
void logRawRegistrations(const char *when)
{
    RAWINPUTDEVICE list[32];
    UINT n = ARRAYSIZE(list);
    UINT got = GetRegisteredRawInputDevices(list,&n,sizeof(RAWINPUTDEVICE));
    if (got == UINT(-1)) { hf->log(owner, "F11 raw: %s: registrations unreadable (%lu)", when, GetLastError()); return; }
    char text[512] = "";
    size_t used = 0;
    for (UINT i = 0; i < got && used < sizeof text - 64; ++i)
        used += (size_t)snprintf(text + used, sizeof text - used, " [page %u usage %u flags 0x%lx target %p]",
                                 list[i].usUsagePage, list[i].usUsage, list[i].dwFlags, (void *)list[i].hwndTarget);
    hf->log(owner, "F11 raw: %s: %u registration(s)%s; the game got %u WM_INPUT and %u buffered packets since the last line",
            when, got, text, gameRawMessages.exchange(0), gameRawBuffered.exchange(0));
}
ULONGLONG lastCursorLog;
unsigned cursorLogs;
void logRect(const char *what, const RECT &r)
{
    hf->log(owner, "F11 input: %s (%ld,%ld)-(%ld,%ld)", what, r.left, r.top, r.right, r.bottom);
}
WNDPROC gameProc;           // the window procedure this menu sits in front of (the game's, or another overlay's)
HWND hookedWindow;
DWORD hookedThread;
bool eatEscapeUp;           // window thread only
decltype(&RegisterRawInputDevices) nextRegister;
// The game's raw input registrations when the menu opened; put back on closing if anything changed them.
RAWINPUTDEVICE snapshot[32];
UINT snapshotCount;
constexpr UINT restoreMessage = WM_APP + 0x4B;

void enqueue(UINT msg, WPARAM w, LPARAM l)
{
    Lock guard(inputLock);
    if (eventCount && msg == WM_MOUSEMOVE && events[eventCount-1].msg == WM_MOUSEMOVE)
        --eventCount;
    if (eventCount == ARRAYSIZE(events)) { eventCount = 0; lostEvents = true; }
    events[eventCount++] = {msg,w,l};
}

BOOL WINAPI clipHook(const RECT *rect)
{
    if (capturing()) { swallowedClips.fetch_add(1); return TRUE; } /* ignored while open, as F8 does */
    return nextClip(rect);
}
// Every SetCursorPos the game makes: in mouse-look it recentres the cursor about once a frame, so the rate before
// opening and after closing the menu tells whether the game left mouse-look around the menu.
std::atomic<unsigned> allWarps{};
BOOL WINAPI warpHook(int x, int y)
{
    allWarps.fetch_add(1);
    if (capturing()) { swallowedWarps.fetch_add(1); return TRUE; } /* ignored while open, as F8 does */
    return nextWarp(x,y);
}

// Return true only for packets owned by the F11 menu. HID controllers and editor
// hotkeys still reach clients that read buffered raw input instead of WM_KEYDOWN.
bool rawEvent(HWND window, const RAWINPUT &in)
{
    if (in.header.dwType == RIM_TYPEHID) return false;
    if (in.header.dwType == RIM_TYPEKEYBOARD) {
        const auto &k = in.data.keyboard;
        if (k.VKey == VK_F8) {
            if (!(k.Flags & RI_KEY_BREAK)) closeMenu();
            return false;
        }
        if (k.VKey == VK_F11) return true; /* the menu's key: never the game's */
        if (k.Message == WM_SYSKEYDOWN || k.Message == WM_SYSKEYUP) return false;
    }
    if (in.header.dwType == RIM_TYPEMOUSE && !(in.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        virtualX.fetch_add(in.data.mouse.lLastX);
        virtualY.fetch_add(in.data.mouse.lLastY);
    }
    if (in.header.dwType == RIM_TYPEMOUSE && rawMouseOnly) {
        USHORT flags = in.data.mouse.usButtonFlags;
        WPARAM delta = MAKEWPARAM(0, in.data.mouse.usButtonData);
        if (flags & RI_MOUSE_WHEEL) enqueue(WM_MOUSEWHEEL,delta,0);
        if (flags & RI_MOUSE_HWHEEL) enqueue(WM_MOUSEHWHEEL,delta,0);
        (void)window;
        LPARAM pos = 0; /* positions come from the virtual pointer */
        struct Button { USHORT down, up; UINT press, release; WPARAM param; };
        static constexpr Button buttons[] = {
            {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, WM_LBUTTONDOWN, WM_LBUTTONUP, 0},
            {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, WM_RBUTTONDOWN, WM_RBUTTONUP, 0},
            {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, WM_MBUTTONDOWN, WM_MBUTTONUP, 0},
            {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON1 << 16},
            {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON2 << 16}
        };
        for (auto &b : buttons) {
            if (flags & b.down) enqueue(b.press,b.param,pos);
            if (flags & b.up) enqueue(b.release,b.param,pos);
        }
    } else if (in.header.dwType == RIM_TYPEKEYBOARD && rawKeyboardOnly) {
        const RAWKEYBOARD &k = in.data.keyboard;
        if (k.VKey == 255) return true;
        if (k.VKey == VK_ESCAPE && !(k.Flags & RI_KEY_BREAK)) { closeMenu(); return true; }
        LPARAM flags = (LPARAM(k.MakeCode) << 16) | ((k.Flags & RI_KEY_E0) ? (1L << 24) : 0);
        if (k.Flags & RI_KEY_BREAK) flags |= LPARAM(0xc0000000u);
        enqueue(k.Message,k.VKey,flags);
        MSG key{}; key.hwnd = window; key.message = k.Message; key.wParam = k.VKey; key.lParam = flags;
        TranslateMessage(&key);
    }
    return true;
}

bool isMenuKey(const RAWINPUT &in)
{
    return in.header.dwType == RIM_TYPEKEYBOARD && in.data.keyboard.VKey == VK_F11;
}

UINT WINAPI rawHook(PRAWINPUT data, PUINT bytes, UINT header)
{
    if (data && capturing()) {
        UINT dropped = nextRaw(data,bytes,header);
        if (dropped != UINT(-1)) eatenRaw.fetch_add(dropped);
        return 0; /* the game gets nothing while the menu is open (as F8) */
    }
    UINT n = nextRaw(data,bytes,header);
    if (data && n && n != UINT(-1)) gameRawBuffered.fetch_add(n);
    return n; /* closed: the game's raw input exactly as Windows gives it */
}

bool inputMessage(UINT message)
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
           message == WM_INPUT || message == WM_MOUSEHOVER || message == WM_MOUSELEAVE || message == WM_NCMOUSEMOVE;
}

void restoreRegistrations(HWND window);

// As HellForge's F8 editor (native/editor/editor.cpp): while the menu is open, keyboard, mouse and raw input go to the
// menu and never to the game; while it is closed, every message goes on to the game unchanged, except the menu's own
// key, F11, which the game never sees (F8 keeps its key too).
LRESULT CALLBACK menuProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == restoreMessage) { restoreRegistrations(window); return 0; }
    if (!stopped.load()) {
        const bool key = message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP;
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (key && w == VK_F11) return 0;
        if (capturing()) {
            if (key && w == VK_F8) {
                if (down) closeMenu(); /* F8 opens the editor: hand the input over and let the key through */
            } else if (key && w == VK_ESCAPE) {
                if (down) { eatEscapeUp = true; closeMenu(); }
                return 0;
            } else if (inputMessage(message)) {
                if (message == WM_INPUT) {
                    if (rawMouseOnly || rawKeyboardOnly) {
                        RAWINPUT raw{};
                        UINT size = sizeof(raw);
                        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER)) != UINT(-1))
                            rawEvent(window,raw);
                    }
                    eatenRaw.fetch_add(1);
                    return DefWindowProcW(window,message,w,l);
                }
                if (message != WM_MOUSEMOVE && message != WM_MOUSEHOVER && message != WM_MOUSELEAVE && message != WM_NCMOUSEMOVE)
                    enqueue(message,w,l);
                return 0;
            } else if (message == WM_CHAR || message == WM_UNICHAR) {
                enqueue(message,w,l);
                return 0;
            }
        } else {
            if (message == WM_INPUT) gameRawMessages.fetch_add(1);
            if (key && w == VK_ESCAPE && !down && eatEscapeUp) { eatEscapeUp = false; return 0; }
        }
    }
    return CallWindowProcW(gameProc,window,message,w,l);
}

void deactivateInput()
{
    HWND window = inputWindow.exchange(nullptr);
    if (!window) return;
    Lock guard(inputLock);
    const bool foreground = GetForegroundWindow()==window;
    if (haveClip && nextClip && foreground) nextClip(clipNull ? nullptr : &savedClip);
    if (!haveClip) hf->log(owner, "F11 input: closed; no clip to restore");
    else if (!foreground) hf->log(owner, "F11 input: closed while the game was not in front; clip not restored");
    else if (clipNull) hf->log(owner, "F11 input: closed; clip restored to none");
    else logRect("closed; clip restored to", savedClip);
    RECT now{};
    POINT pt{};
    if (GetClipCursor(&now)) logRect("clip after closing", now);
    GetCursorPos(&pt);
    hf->log(owner, "F11 input: cursor %ld,%ld; while open the game called SetCursorPos x%u and ClipCursor x%u (passed), %u raw packets went to the menu",
            pt.x, pt.y, swallowedWarps.exchange(0), swallowedClips.exchange(0), eatenRaw.exchange(0));
    haveClip = false;
    eventCount = 0;
    lostEvents = true;
    if (snapshotCount) PostMessageW(window,restoreMessage,0,0); /* compared and put back on the window's thread */
}

bool sameRegistration(const RAWINPUTDEVICE &a, const RAWINPUTDEVICE &b)
{
    return a.usUsagePage == b.usUsagePage && a.usUsage == b.usUsage && a.dwFlags == b.dwFlags && a.hwndTarget == b.hwndTarget;
}

void restoreRegistrations(HWND window)
{
    (void)window;
    if (!snapshotCount || capturing()) return;
    RAWINPUTDEVICE now[32];
    UINT n = ARRAYSIZE(now);
    UINT got = GetRegisteredRawInputDevices(now,&n,sizeof(RAWINPUTDEVICE));
    if (got == UINT(-1)) return;
    UINT changed = 0;
    for (UINT i = 0; i < snapshotCount; ++i) {
        bool present = false;
        for (UINT j = 0; j < got && !present; ++j) present = sameRegistration(snapshot[i],now[j]);
        if (present) continue;
        ++changed;
        const BOOL ok = (nextRegister ? nextRegister : RegisterRawInputDevices)(&snapshot[i],1,sizeof(RAWINPUTDEVICE));
        hf->log(owner, "F11 raw: put back the game's registration page %u usage %u flags 0x%lx target %p: %s",
                snapshot[i].usUsagePage, snapshot[i].usUsage, snapshot[i].dwFlags, (void *)snapshot[i].hwndTarget,
                ok ? "done" : "refused");
    }
    if (!changed) hf->log(owner, "F11 raw: closing: the game's raw input registrations are as they were");
}

// Every change to the raw input registrations, with who made it (to find what stops the game's mouse input).
BOOL WINAPI registerHook(PCRAWINPUTDEVICE devices, UINT count, UINT size)
{
    const BOOL ok = nextRegister(devices,count,size);
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(__builtin_return_address(0)),&module))
        GetModuleFileNameW(module,path,MAX_PATH);
    const wchar_t *name = wcsrchr(path,L'\\');
    char caller[MAX_PATH];
    WideCharToMultiByte(CP_UTF8,0,name ? name + 1 : path,-1,caller,sizeof caller,nullptr,nullptr);
    for (UINT i = 0; devices && i < count && i < 8; ++i)
        hf->log(owner, "F11 raw: RegisterRawInputDevices from %s (thread %lu, menu %s): page %u usage %u flags 0x%lx target %p -> %s",
                caller, GetCurrentThreadId(), visible.load() ? "open" : "closed", devices[i].usUsagePage, devices[i].usUsage,
                devices[i].dwFlags, (void *)devices[i].hwndTarget, ok ? "ok" : "failed");
    return ok;
}


bool installMessageHook(HWND window)
{
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(window,&process);
    if (!thread || process != GetCurrentProcessId()) return false;
    if (hookedWindow == window && thread == hookedThread && gameProc) return true;
    if (hookedWindow) return false; /* one window only */
    WNDPROC previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(menuProc)));
    if (!previous) return false;
    gameProc = previous;
    hookedWindow = window;
    hookedThread = thread;
    return true;
}

bool activateInput(HWND window)
{
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(window,&process);
    if (!thread || process != GetCurrentProcessId()) return false;
    if (inputWindow.load() == window && thread == hookedThread) return true;
    deactivateInput();
    if (!installMessageHook(window)) return false;
    RAWINPUTDEVICE registrations[32];
    UINT n = ARRAYSIZE(registrations);
    UINT got = GetRegisteredRawInputDevices(registrations,&n,sizeof(RAWINPUTDEVICE));
    snapshotCount = got == UINT(-1) ? 0 : got;
    if (snapshotCount) memcpy(snapshot,registrations,snapshotCount * sizeof(RAWINPUTDEVICE));
    rawMouseOnly = rawKeyboardOnly = false;
    for (UINT i=0; got != UINT(-1) && i<got; ++i) {
        const auto &r = registrations[i];
        if (r.usUsagePage != 1 || (r.dwFlags & RIDEV_NOLEGACY) != RIDEV_NOLEGACY) continue;
        if (r.usUsage == 2) rawMouseOnly = true;
        if (r.usUsage == 6) rawKeyboardOnly = true;
    }
    Lock guard(inputLock);
    haveClip = false; /* nothing is put back on closing: the game clips again by itself */
    clipNull = false;
    if (nextClip) nextClip(nullptr);
    inputWindow.store(window);
    RECT client{};
    GetClientRect(window, &client);
    POINT start{};
    if (!GetCursorPos(&start) || !ScreenToClient(window,&start) || start.x < 0 || start.y < 0 ||
        start.x >= client.right || start.y >= client.bottom)
        start = {client.right / 2, client.bottom / 2};
    virtualX.store(start.x);
    virtualY.store(start.y);
    hf->log(owner, "F11 input: opened; raw-only mouse %d, raw-only keyboard %d, client %ldx%ld",
            rawMouseOnly.load() ? 1 : 0, rawKeyboardOnly.load() ? 1 : 0, client.right, client.bottom);
    swallowedWarps.store(0); swallowedClips.store(0); eatenRaw.store(0);
    lastCursorLog = 0; cursorLogs = 0;
    return true;
}

void inputFrame(HWND window)
{
    InputEvent pending[ARRAYSIZE(events)];
    unsigned n;
    ImGuiIO &io = ImGui::GetIO();
    {
        Lock guard(inputLock);
        n = eventCount;
        memcpy(pending,events,n * sizeof(*events));
        eventCount = 0;
        if (lostEvents) { io.ClearEventsQueue(); io.ClearInputKeys(); io.ClearInputMouse(); lostEvents = false; }
    }
    for (unsigned i=0; i<n; ++i) {
        const auto &e = pending[i];
        // The Win32 backend calls SetCapture on button down. Replay buttons directly:
        // Present and the owner of the foreground input queue can be different threads.
        int button = -1;
        switch (e.msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: button=0; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: button=1; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: button=2; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: button=HIWORD(e.w)==XBUTTON1 ? 3 : 4; break;
        }
        if (e.msg == WM_MOUSEMOVE) continue; /* the position is read from the system cursor below */
        if (button >= 0) {
            POINT at{};
            if (GetCursorPos(&at) && ScreenToClient(window,&at)) io.AddMousePosEvent(float(at.x),float(at.y));
            io.AddMouseButtonEvent(button,e.msg!=WM_LBUTTONUP && e.msg!=WM_RBUTTONUP &&
                e.msg!=WM_MBUTTONUP && e.msg!=WM_XBUTTONUP);
        } else ImGui_ImplWin32_WndProcHandler(window,e.msg,e.w,e.l);
    }
    POINT pt;
    if (GetCursorPos(&pt)) {
        const ULONGLONG now = GetTickCount64();
        if (cursorLogs < 30 && now - lastCursorLog >= 1000) {
            RECT clip{};
            GetClipCursor(&clip);
            hf->log(owner, "F11 input: menu pointer %ld,%ld, system cursor %ld,%ld, clip (%ld,%ld)-(%ld,%ld), game SetCursorPos x%u ClipCursor x%u (passed), raw to menu %u",
                    virtualX.load(), virtualY.load(), pt.x, pt.y, clip.left, clip.top, clip.right, clip.bottom,
                    swallowedWarps.load(), swallowedClips.load(), eatenRaw.load());
            lastCursorLog = now;
            ++cursorLogs;
        }
    }
    if (GetCursorPos(&pt) && ScreenToClient(window,&pt)) io.AddMousePosEvent(float(pt.x),float(pt.y));
    if (rawMouseOnly) {
        const int keys[] = {VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
        for (int i=0; i<5; ++i) io.AddMouseButtonEvent(i,(GetAsyncKeyState(keys[i]) & 0x8000)!=0);
    }
}

constexpr UINT framesInFlight = 3;
struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> buffer;
    UINT64 fenceValue = 0;
};
struct Renderer {
    HWND window{};
    UINT64 chain{};
    DXGI_FORMAT format{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue, upload;
    ComPtr<ID3D12DescriptorHeap> rtvs, srv;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    ImGuiContext *context{};
    bool platform{}, backend{}, unsignaled{}, broken{};
    Slot slots[framesInFlight];
    UINT frame{};
    UINT64 submitted{};
    ~Renderer() {
        if (context) {
            ImGuiContext *previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(context);
            if (backend) ImGui_ImplDX12_Shutdown();
            if (platform) ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous == context ? nullptr : previous);
        }
        if (event) CloseHandle(event);
    }
};
// No static COM/synchronization destructors during process detach.
Renderer *renderer;
struct Context {
    ImGuiContext *previous;
    explicit Context(ImGuiContext *c) : previous(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(c); }
    ~Context() { ImGui::SetCurrentContext(previous); }
};

void addFont(float pixels)
{
    auto &io = ImGui::GetIO();
    wchar_t windows[MAX_PATH];
    UINT len = GetWindowsDirectoryW(windows,ARRAYSIZE(windows));
    if (!len || len >= ARRAYSIZE(windows)) return;
    const wchar_t *fonts[] = {L"msyh.ttc",L"msjh.ttc",L"simsun.ttc",L"mingliu.ttc"};
    for (auto name : fonts) {
        wchar_t path[MAX_PATH + 32];
        swprintf(path,ARRAYSIZE(path),L"%ls\\Fonts\\%ls",windows,name);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
        char utf8[sizeof(path)*2];
        if (!WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,sizeof(utf8),nullptr,nullptr)) continue;
        ImFontConfig cfg;
        cfg.OversampleH = cfg.OversampleV = 1;
        io.Fonts->TexDesiredWidth = 4096;
        // ChineseFull plus the catalog's circled numerals and favorite-planet star.
        static const ImWchar ranges[] = {0x20,0xff,0x2000,0x206f,0x2460,0x2463,0x2605,0x2605,
            0x3000,0x30ff,0x31f0,0x31ff,0x4e00,0x9fff,0xff00,0xffef,0xfffd,0xfffd,0};
        if (io.Fonts->AddFontFromFileTTF(utf8,pixels,&cfg,ranges)) return;
    }
    hf->log(owner,"F11: no supported Windows Chinese font found; using the ImGui default");
}

bool initRenderer(Renderer &r, ID3D12CommandQueue *queue, ID3D12Device *device, HWND window,
                  UINT64 chain, DXGI_FORMAT format)
{
    r.queue = queue; r.device = device; r.window = window; r.chain = chain; r.format = format;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = framesInFlight;
    if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&r.rtvs)))) return false;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&r.srv))) ||
        FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&r.fence)))) return false;
    r.event = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!r.event) return false;
    for (auto &s : r.slots) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator))) ||
            FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,
                                             IID_PPV_ARGS(&s.list))) || FAILED(s.list->Close())) return false;
    }
    // 1.91.8 waits synchronously on InitInfo.CommandQueue during font upload. A game
    // queue may itself be waiting for a signal that will be issued after this hook returns.
    D3D12_COMMAND_QUEUE_DESC q{};
    if (FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&r.upload)))) return false;

    Context restore(nullptr);
    IMGUI_CHECKVERSION();
    r.context = ImGui::CreateContext();
    if (!r.context) return false;
    ImGui::SetCurrentContext(r.context);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = true;
    float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    if (dpi < 1.f) dpi = 1.f;
    if (dpi > 2.f) dpi = 2.f;
    ImGui::GetStyle().ScaleAllSizes(dpi);
    addFont(18.f * dpi);
    r.platform = ImGui_ImplWin32_Init(window);
    if (!r.platform) return false;
    ImGui_ImplDX12_InitInfo init;
    init.Device = device; init.CommandQueue = r.upload.Get(); init.NumFramesInFlight = framesInFlight;
    init.RTVFormat = format; init.SrvDescriptorHeap = r.srv.Get();
    init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *i,D3D12_CPU_DESCRIPTOR_HANDLE *c,D3D12_GPU_DESCRIPTOR_HANDLE *g) {
        *c = i->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        *g = i->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    };
    init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_GPU_DESCRIPTOR_HANDLE) {};
    r.backend = ImGui_ImplDX12_Init(&init);
    return r.backend && ImGui_ImplDX12_CreateDeviceObjects();
}

bool drainFrames()
{
    if (!renderer) return true;
    auto &r = *renderer;
    if (r.submitted && SUCCEEDED(r.device->GetDeviceRemovedReason())) {
        if (r.unsignaled && FAILED(r.queue->Signal(r.fence.Get(),r.submitted))) return false;
        r.unsignaled = false;
        if (r.fence->GetCompletedValue() < r.submitted) {
            if (FAILED(r.fence->SetEventOnCompletion(r.submitted,r.event))) return false;
            WaitForSingleObject(r.event,1000);
            if (r.fence->GetCompletedValue() < r.submitted) return false;
        }
    }
    for (auto &s : r.slots) s.buffer.Reset();
    return true;
}

bool retire()
{
    if (!drainFrames()) return false;
    deactivateInput();
    delete renderer; renderer = nullptr;
    return true;
}

thread_local bool ownWork, insidePresent;
struct Flag {
    bool &flag;
    explicit Flag(bool &f) : flag(f) { flag=true; }
    ~Flag() { flag=false; }
};
bool sameObject(IUnknown *a, IUnknown *b)
{
    if (a == b) return true;
    ComPtr<IUnknown> x,y;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
           SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x.Get()==y.Get();
}

// Called with graphicsLock held. The allocator/RTV and ImGui's private vertex/index
// buffers rotate by successful RenderDrawData calls, independently of DXGI's buffer index.
bool draw(ID3D12CommandQueue *queue, ID3D12Resource *buffer, HWND window, UINT64 cookie)
{
    if (!visible.load() || stopped.load() || GetForegroundWindow()!=window) return false;
    Flag self(ownWork);
    auto desc = buffer->GetDesc();
    ComPtr<ID3D12Device> device, queueDevice;
    if (desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count!=1 ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        FAILED(buffer->GetDevice(IID_PPV_ARGS(&device))) || FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
        queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT || device->GetNodeCount()!=1 ||
        !sameObject(device.Get(),queueDevice.Get()) || FAILED(device->GetDeviceRemovedReason())) return false;
    if (renderer && (renderer->broken || renderer->unsignaled || renderer->chain!=cookie || renderer->format!=desc.Format ||
                     !sameObject(renderer->queue.Get(),queue)) && !retire()) return false;
    if (!renderer) {
        renderer = new (std::nothrow) Renderer{};
        if (!renderer) return false;
        if (!initRenderer(*renderer,queue,device.Get(),window,cookie,desc.Format)) {
            hf->log(owner,"F11: Win32/D3D12 initialization failed");
            retire(); closeMenu(); return false;
        }
    }
    Renderer &r = *renderer;
    Slot &s = r.slots[r.frame];
    if (r.broken || r.unsignaled || r.fence->GetCompletedValue()<s.fenceValue) return false;
    if (!activateInput(window)) { closeMenu(); return false; }
    Context current(r.context);
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    inputFrame(window);
    ImGui::NewFrame();
    bool open = visible.load();
    if (open) host_ui_draw(&open);
    ImGui::Render();
    if (!open) { closeMenu(); deactivateInput(); return false; }
    ImDrawData *data = ImGui::GetDrawData();
    if (!visible.load() || stopped.load() || !data || !data->TotalVtxCount ||
        data->DisplaySize.x<=0 || data->DisplaySize.y<=0) return false;
    // The pinned backend expects framebuffer pixels. Input and host widgets use client coordinates.
    ImVec2 scale(float(desc.Width)/data->DisplaySize.x,float(desc.Height)/data->DisplaySize.y);
    if (scale.x!=1.f || scale.y!=1.f) {
        for (auto list : data->CmdLists)
            for (auto &v : list->VtxBuffer) { v.pos.x *= scale.x; v.pos.y *= scale.y; }
        data->ScaleClipRects(scale);
        data->DisplaySize = ImVec2(float(desc.Width),float(desc.Height));
    }
    if (FAILED(s.allocator->Reset()) || FAILED(s.list->Reset(s.allocator.Get(),nullptr))) {
        r.broken=true; closeMenu(); return false;
    }
    s.buffer = buffer;
    auto rtv = r.rtvs->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(r.frame)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(buffer,nullptr,rtv);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buffer,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};
    s.list->ResourceBarrier(1,&barrier);
    s.list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    ID3D12DescriptorHeap *heaps[] = {r.srv.Get()};
    s.list->SetDescriptorHeaps(1,heaps);
    ImGui_ImplDX12_RenderDrawData(data,s.list.Get());
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    s.list->ResourceBarrier(1,&barrier);
    if (FAILED(s.list->Close())) { r.broken=true; closeMenu(); return false; }
    ID3D12CommandList *lists[] = {s.list.Get()};
    queue->ExecuteCommandLists(1,lists);
    s.fenceValue = ++r.submitted;
    r.unsignaled = FAILED(queue->Signal(r.fence.Get(),r.submitted));
    r.frame = (r.frame+1)%framesInFlight;
    if (r.unsignaled) { hf->log(owner,"F11: queue Signal failed"); closeMenu(); }
    return true;
}

// Store the factory's actual presentation queue on the DXGI object. DXGI owns the
// private interface reference and releases it with the chain; we never pin the chain.
void rememberQueue(IDXGISwapChain *chain, IUnknown *arg)
{
    ComPtr<ID3D12CommandQueue> queue;
    if (arg && SUCCEEDED(arg->QueryInterface(IID_PPV_ARGS(&queue))) &&
        queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT)
        chain->SetPrivateDataInterface(queueKey,queue.Get());
}

HRESULT STDMETHODCALLTYPE createHook(IDXGIFactory *f,IUnknown *d,DXGI_SWAP_CHAIN_DESC *desc,IDXGISwapChain **out)
{
    HRESULT hr = nextCreate(f,d,desc,out);
    if (ready.load() && !stopped.load() && SUCCEEDED(hr) && out && *out) rememberQueue(*out,d);
    return hr;
}
HRESULT STDMETHODCALLTYPE createHwndHook(IDXGIFactory2 *f,IUnknown *d,HWND w,const DXGI_SWAP_CHAIN_DESC1 *desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,IDXGIOutput *output,IDXGISwapChain1 **out)
{
    HRESULT hr = nextCreateHwnd(f,d,w,desc,fs,output,out);
    if (ready.load() && !stopped.load() && SUCCEEDED(hr) && out && *out) rememberQueue(*out,d);
    return hr;
}

UINT64 serial, activeChain;
HWND activeWindow;
unsigned resizing;
std::atomic<UINT64> tracking{};
UINT drawnBuffers;
struct BufferTag { UINT64 generation; UINT index; };
struct Transition { BufferTag tag; ID3D12Resource *buffer; };

// Late installation: associate a command list with the actual DXGI buffer it moves
// back to PRESENT. Append to that submission before returning to the engine, so a
// subsequent cross-queue Signal covers our work too. Never guess from queue activity.
void STDMETHODCALLTYPE barrierHook(ID3D12GraphicsCommandList *list,UINT n,const D3D12_RESOURCE_BARRIER *barriers)
{
    UINT64 generation = tracking.load();
    if (generation && !ownWork && !stopped.load() && visible.load()) {
        for (UINT i=0; i<n; ++i) {
            const auto &b = barriers[i];
            if (b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
            if (b.Transition.StateBefore!=D3D12_RESOURCE_STATE_PRESENT &&
                b.Transition.StateAfter!=D3D12_RESOURCE_STATE_PRESENT) continue;
            BufferTag tag{};
            if (!readTag(b.Transition.pResource,bufferKey,tag) || tag.generation!=generation) continue;
            bool end = b.Transition.StateAfter==D3D12_RESOURCE_STATE_PRESENT &&
                       b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
            Transition transition{tag,end ? b.Transition.pResource : nullptr};
            list->SetPrivateData(listKey,sizeof(transition),&transition);
        }
    }
    nextBarrier(list,n,barriers);
}
HRESULT STDMETHODCALLTYPE resetHook(ID3D12GraphicsCommandList *list,ID3D12CommandAllocator *a,ID3D12PipelineState *p)
{
    HRESULT hr = nextReset(list,a,p);
    if (SUCCEEDED(hr) && tracking.load() && !ownWork && !stopped.load()) list->SetPrivateData(listKey,0,nullptr);
    return hr;
}
void STDMETHODCALLTYPE executeHook(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *lists)
{
    nextExecute(q,n,lists);
    UINT64 generation = tracking.load();
    if (!generation || ownWork || !visible.load() || stopped.load() ||
        q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    Transition final{};
    for (UINT i=0; i<n; ++i) {
        Transition t{};
        if (readTag(lists[i],listKey,t) && t.tag.generation==generation) final=t;
    }
    if (!final.buffer || final.tag.index>=DXGI_MAX_SWAP_CHAIN_BUFFERS) return;
    Lock guard(graphicsLock);
    UINT bit = 1u << final.tag.index;
    if (tracking.load()==generation && !resizing && !(drawnBuffers & bit) &&
        draw(q,final.buffer,activeWindow,activeChain)) drawnBuffers |= bit;
}

bool inspectPresent(IDXGISwapChain *chain,UINT flags)
{
    if (stopped.load() || !ready.load() || (flags & DXGI_PRESENT_TEST)) return false;
    Lock guard(graphicsLock);
    if (renderer && renderer->fence && !renderer->unsignaled) {
        UINT64 done = renderer->fence->GetCompletedValue();
        for (auto &s : renderer->slots) if (s.fenceValue <= done) s.buffer.Reset();
    }
    if (!hookedWindow) {
        /* keep F11 from the game from the first press on */
        DXGI_SWAP_CHAIN_DESC early{};
        if (SUCCEEDED(chain->GetDesc(&early)) && early.OutputWindow && GetAncestor(early.OutputWindow,GA_ROOT)==early.OutputWindow &&
            installMessageHook(early.OutputWindow))
            hf->log(owner, "F11: the menu sits in front of the game's window procedure (as F8); the game gets no F11 from now on");
    }
    if (!visible.load() || stopped.load()) {
        tracking.store(0); deactivateInput(); return false;
    }
    if (resizing || (flags & DXGI_PRESENT_DO_NOT_WAIT)) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow) return false;
    HWND w = desc.OutputWindow;
    if (w!=GetForegroundWindow() || !IsWindowVisible(w) || IsIconic(w) ||
        GetAncestor(w,GA_ROOT)!=w || (GetWindowLongPtrW(w,GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
        if (w==activeWindow) deactivateInput();
        return false;
    }
    DWORD pid=0;
    GetWindowThreadProcessId(w,&pid);
    ComPtr<IDXGISwapChain3> flip;
    ComPtr<ID3D12Device> device;
    if (pid!=GetCurrentProcessId() || FAILED(chain->QueryInterface(IID_PPV_ARGS(&flip))) ||
        FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || device->GetNodeCount()!=1 ||
        !(desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) || desc.BufferCount>DXGI_MAX_SWAP_CHAIN_BUFFERS ||
        !desc.BufferCount || desc.SampleDesc.Count!=1 || (desc.Flags & DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED))
        return false;
    UINT64 cookie{};
    if (!readTag(chain,chainKey,cookie)) {
        cookie = ++serial;
        if (FAILED(chain->SetPrivateData(chainKey,sizeof(cookie),&cookie))) return false;
    }
    if (activeChain!=cookie) {
        tracking.store(0); drawnBuffers=0;
        if (!retire()) return false;
        activeChain=cookie; activeWindow=w;
    }
    UINT index = flip->GetCurrentBackBufferIndex();
    if (index>=desc.BufferCount) return false;
    // GetPrivateData on SetPrivateDataInterface returns an AddRef'd pointer.
    ComPtr<ID3D12CommandQueue> queue;
    UINT bytes = sizeof(ID3D12CommandQueue *);
    if (SUCCEEDED(chain->GetPrivateData(queueKey,&bytes,queue.GetAddressOf())) && queue) {
        tracking.store(0);
        ComPtr<ID3D12Resource> b;
        return SUCCEEDED(chain->GetBuffer(index,IID_PPV_ARGS(&b))) && draw(queue.Get(),b.Get(),w,cookie);
    }
    if (!tracking.load()) {
        UINT64 generation = ++serial;
        for (UINT i=0; i<desc.BufferCount; ++i) {
            ComPtr<ID3D12Resource> b;
            BufferTag tag{generation,i};
            if (FAILED(chain->GetBuffer(i,IID_PPV_ARGS(&b))) ||
                FAILED(b->SetPrivateData(bufferKey,sizeof(tag),&tag))) return false;
        }
        drawnBuffers=0; tracking.store(generation);
    }
    UINT mask = 1u << index;
    bool changed = (drawnBuffers & mask)!=0;
    drawnBuffers &= ~mask;
    return changed;
}
HRESULT STDMETHODCALLTYPE presentHook(IDXGISwapChain *chain,UINT interval,UINT flags)
{
    if (insidePresent) return nextPresent(chain,interval,flags);
    Flag nested(insidePresent);
    inspectPresent(chain,flags);
    return nextPresent(chain,interval,flags);
}
HRESULT STDMETHODCALLTYPE present1Hook(IDXGISwapChain1 *chain,UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS *p)
{
    if (insidePresent) return nextPresent1(chain,interval,flags,p);
    Flag nested(insidePresent);
    bool changed = inspectPresent(chain,flags);
    DXGI_PRESENT_PARAMETERS full{};
    return nextPresent1(chain,interval,flags,changed && p ? &full : p);
}

struct Resizing {
    explicit Resizing(IDXGISwapChain *chain) {
        Lock guard(graphicsLock);
        ++resizing;
        UINT64 cookie{};
        if (readTag(chain,chainKey,cookie) && cookie==activeChain) {
            tracking.store(0); drawnBuffers=0;
            if (!drainFrames()) hf->log(owner,"F11: outstanding GPU work during resize; retaining its resources");
        }
    }
    ~Resizing() { Lock guard(graphicsLock); --resizing; }
};
HRESULT STDMETHODCALLTYPE resizeHook(IDXGISwapChain *c,UINT n,UINT w,UINT h,DXGI_FORMAT fmt,UINT flags)
{
    if (stopped.load() || !ready.load()) return nextResize(c,n,w,h,fmt,flags);
    Resizing guard(c);
    return nextResize(c,n,w,h,fmt,flags);
}
HRESULT STDMETHODCALLTYPE resize1Hook(IDXGISwapChain3 *c,UINT n,UINT w,UINT h,DXGI_FORMAT fmt,UINT flags,
                                    const UINT *nodes,IUnknown *const *queues)
{
    if (stopped.load() || !ready.load()) return nextResize1(c,n,w,h,fmt,flags,nodes,queues);
    Resizing guard(c);
    HRESULT hr = nextResize1(c,n,w,h,fmt,flags,nodes,queues);
    if (SUCCEEDED(hr) && queues) {
        c->SetPrivateDataInterface(queueKey,nullptr);
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(c->GetDesc(&d)) && d.BufferCount) {
            bool one = true;
            for (UINT i=1; i<d.BufferCount; ++i) if (!sameObject(queues[0],queues[i])) one=false;
            if (one) rememberQueue(c,queues[0]);
        }
    }
    return hr;
}

template<class T> uintptr_t method(T *object,unsigned index)
{
    return (*reinterpret_cast<uintptr_t **>(object))[index];
}
template<class T> bool hook(const char *name,uintptr_t target,T detour,T &next)
{
    if (target && hf->hook(owner,target,reinterpret_cast<void *>(detour),reinterpret_cast<void **>(&next))==0)
        return true;
    hf->log(owner,"F11: could not chain %s",name);
    return false;
}
bool install()
{
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<IDXGISwapChain1> probe;
    ComPtr<IDXGISwapChain3> chain;
    D3D12_COMMAND_QUEUE_DESC q{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))) ||
        FAILED(list->Close())) return false;
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,0,0,8,8,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (!w) return false;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width=d.Height=8; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
    d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount=2; d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue.Get(),w,&d,nullptr,nullptr,&probe);
    if (SUCCEEDED(hr)) hr=probe.As(&chain);
    bool ok=false;
    if (SUCCEEDED(hr)) {
        ok = hook("CreateSwapChain",method(factory.Get(),10),createHook,nextCreate) &&
             hook("CreateSwapChainForHwnd",method(factory.Get(),15),createHwndHook,nextCreateHwnd) &&
             hook("ExecuteCommandLists",method(queue.Get(),10),executeHook,nextExecute) &&
             hook("ResourceBarrier",method(list.Get(),26),barrierHook,nextBarrier) &&
             hook("Reset",method(list.Get(),10),resetHook,nextReset) &&
             hook("ResizeBuffers",method(chain.Get(),13),resizeHook,nextResize) &&
             hook("ResizeBuffers1",method(chain.Get(),39),resize1Hook,nextResize1) &&
             hook("Present",method(chain.Get(),8),presentHook,nextPresent) &&
             hook("Present1",method(chain.Get(),22),present1Hook,nextPresent1);
        HMODULE user = GetModuleHandleW(L"user32.dll");
        ok = ok && hook("ClipCursor",reinterpret_cast<uintptr_t>(GetProcAddress(user,"ClipCursor")),clipHook,nextClip) &&
             hook("SetCursorPos",reinterpret_cast<uintptr_t>(GetProcAddress(user,"SetCursorPos")),warpHook,nextWarp) &&
             hook("GetRawInputBuffer",reinterpret_cast<uintptr_t>(GetProcAddress(user,"GetRawInputBuffer")),rawHook,nextRaw);
        if (ok && !hook("RegisterRawInputDevices",reinterpret_cast<uintptr_t>(GetProcAddress(user,"RegisterRawInputDevices")),
                        registerHook,nextRegister))
            nextRegister = nullptr;
    }
    chain.Reset(); probe.Reset(); DestroyWindow(w);
    return ok;
}

void tick(HfMod *,float,void *)
{
    static bool attempted;
    if (stopped.load()) return;
    if (!attempted) {
        attempted=true;
        bool ok=install();
        ready.store(ok);
        hf->log(owner,ok ? "F11: D3D12 presentation hooks installed" : "F11: D3D12 hook installation failed");
    }
    if (!ready.load()) return;
    if (capturing() && (GetAsyncKeyState(VK_F8) & 0x8000)) closeMenu();
    {
        /* mouse-look before opening and after closing (see allWarps) */
        static bool wasOpen;
        static ULONGLONG second, closedAt;
        static unsigned mark, lastSecond, previousSecond, atClose, reports;
        const ULONGLONG now = GetTickCount64();
        if (now - second >= 1000) {
            const unsigned total = allWarps.load();
            previousSecond = lastSecond;
            lastSecond = total - mark;
            mark = total;
            second = now;
        }
        const bool open = visible.load();
        if (open && !wasOpen) logRawRegistrations("opening");
        if (!open && wasOpen) logRawRegistrations("closing");
        if (open && !wasOpen)
            hf->log(owner, "F11 mouse-look: before opening, the game recentred the cursor %u and %u times in the last two seconds (0 = the game was not in mouse-look)",
                    previousSecond, lastSecond);
        if (!open && wasOpen) { closedAt = now; atClose = allWarps.load(); reports = 0; }
        if (!open && closedAt && reports < 2 && now - closedAt >= (reports ? 3000u : 1000u)) {
            hf->log(owner, "F11 mouse-look: %s after closing, the game has recentred the cursor %u times (0 = it has not gone back to mouse-look)",
                    reports ? "3 s" : "1 s", allWarps.load() - atClose);
            if (reports) logRawRegistrations("3 s after closing");
            if (++reports == 2) closedAt = 0;
        }
        wasOpen = open;
    }
    if (!visible.load() && inputWindow.load() && TryAcquireSRWLockExclusive(&graphicsLock)) {
        if (!visible.load()) { deactivateInput(); tracking.store(0); }
        ReleaseSRWLockExclusive(&graphicsLock);
    }
}
} // namespace

extern "C" bool overlay_start(const HfApi *api,HfMod *mod)
{
    if (stopped.load() || !api || !mod || api->version<16 ||
        api->size<offsetof(HfApi,on_game_thread)+sizeof(api->on_game_thread) ||
        !api->hook || !api->on_game_thread || !api->log) return false;
    if (hf) return hf==api && owner==mod;
    hf=api; owner=mod;
    if (api->on_game_thread(mod,tick,nullptr)) return true;
    hf=nullptr; owner=nullptr;
    return false;
}
extern "C" void overlay_set_visible(bool show)
{
    if (!stopped.load()) visible.store(show);
}
extern "C" void overlay_stop(void)
{
    stopped.store(true);
    visible.store(false);
}
