// SPDX-License-Identifier: CC0-1.0
//
// NFSU2 Fixed Hood Cam — rigidly attach the hood camera to the car.
//
// Targets the US v1.2 SPEED2.EXE (sha256
// bc34932b36bb19194000690ec26edf964176856c31fb0978e01e61ee814eee9d).
//
// The plugin hooks the look-at call in TrackCarCameraMover::Update.  In rigid
// mode it replaces the game's camera position/target with a fixed car-local
// mount captured on the first frame, removing all positional and rotational
// inertia.  HorizonLock chooses between world up (level horizon) and car up
// (camera rolls with the car).
// Nothing is changed on disk; the EXE is patched in memory only.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>

// ---------------------------------------------------------------------------
// Game coordinates (US v1.2; EXE base is normally 0x400000).
// ---------------------------------------------------------------------------
static const uint32_t OFF_PLAYER_PTR = 0x4900AC; // 0x8900AC: Player* array
static const uint32_t OFF_ACTIVE_CAM = 0x238;    // Player+0x238: current mover
static const uint32_t OFF_POV_TYPE   = 0x4C;     // mover+0x13*4: POV type (4 = hood)
static const uint32_t OFF_CALL_POS   = 0x53B44;  // 0x453B44: call FUN_005BA9A0
static const uint32_t OFF_ORIG_POS   = 0x1BA9A0; // 0x5BA9A0: look-at matrix
static const uint32_t OFF_CAR        = 0x2C;     // mover+0x2C: car object
static const uint32_t OFF_CAR_POS    = 0x50;     // car+0x50: world position
static const uint32_t OFF_CAR_MATRIX = 0x90;     // car+0x90: body-to-world rows

// Player car audio pan update.  The five CALLs are the layer updates in
// EAXCar/CARSFX_PlayerEngine; the helper centers only object index 0.
static const uint32_t OFF_AUDIO_PAN_FN = 0x5DE60; // 0x45DE60
static const uint32_t OFF_AUDIO_PAN_CALLS[] = {
    0x5DF73, 0x5DFE0, 0x5E02E, 0x5E068, 0x5E0A0
};
static const uint32_t OFF_AUDIO_PAN_GLOBAL = 0x42BB80; // 0x82BB80

static uintptr_t g_base = 0;
static bool      g_enabled = true;
static bool      g_onlyHood = true;
static bool      g_removeMovement = true;
static bool      g_horizonLock = false; // false = car up (fixed to car), true = world up
static bool      g_centerPlayerAudio = true;
static char      g_iniPath[MAX_PATH] = { 0 };
static char      g_logPath[MAX_PATH] = { 0 };

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void LogLine(const char* fmt, ...) {
    if (!g_logPath[0]) return;
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static void ReadConfig() {
    g_enabled  = GetPrivateProfileIntA("General", "Enabled", 1, g_iniPath) != 0;
    g_onlyHood = GetPrivateProfileIntA("General", "OnlyHood", 1, g_iniPath) != 0;

    g_removeMovement = GetPrivateProfileIntA("Camera", "RemoveMovement", 1, g_iniPath) != 0;
    g_horizonLock    = GetPrivateProfileIntA("Camera", "HorizonLock", 0, g_iniPath) != 0;

    g_centerPlayerAudio = GetPrivateProfileIntA("Audio", "CenterPlayer", 1, g_iniPath) != 0;
}

static void BuildPaths(HMODULE module) {
    char modulePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(module, modulePath, MAX_PATH);
    char* lastSlash = modulePath;
    for (char* p = modulePath; *p; ++p) {
        if (*p == '\\' || *p == '/') lastSlash = p + 1;
    }
    *lastSlash = '\0';
    lstrcpyA(g_iniPath, modulePath);
    lstrcatA(g_iniPath, "NFSU2FixedHoodCam.ini");
    lstrcpyA(g_logPath, modulePath);
    lstrcatA(g_logPath, "NFSU2FixedHoodCam.log");

    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringA("General", "Enabled", "1", g_iniPath);
        WritePrivateProfileStringA("General", "OnlyHood", "1", g_iniPath);
        WritePrivateProfileStringA("General", "DebugLog", "0", g_iniPath);
        WritePrivateProfileStringA("Camera", "RemoveMovement", "1", g_iniPath);
        WritePrivateProfileStringA("Camera", "HorizonLock", "0", g_iniPath);
        WritePrivateProfileStringA("Audio", "CenterPlayer", "1", g_iniPath);
    }
}

// ---------------------------------------------------------------------------
// Camera path hook.  The game builds the hood camera position in world space
// from a local mount plus dynamic accel/brake movement.  This helper captures
// the mount once and rebuilds it from the car transform, removing the game's
// positional and rotational inertia.  HorizonLock selects world up or car up.
// ---------------------------------------------------------------------------
static inline float Dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void Normalize3(float* v) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1.0e-6f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

extern "C" void __cdecl AdjustCameraPosition(void* mover, void* targetPtr, void* posPtr, void* upPtr) {
    if (!g_enabled || !g_removeMovement || !mover || !targetPtr || !posPtr || !upPtr) return;

    uint8_t* m = (uint8_t*)mover;
    if (g_onlyHood && *(int*)(m + OFF_POV_TYPE) != 4) return;

    uint32_t car = *(uint32_t*)(m + OFF_CAR);
    if (!car) return;
    float* carPos = (float*)((uint8_t*)car + OFF_CAR_POS);
    float* fwd = (float*)((uint8_t*)car + OFF_CAR_MATRIX);
    float* right = (float*)((uint8_t*)car + OFF_CAR_MATRIX + 0x10);
    float* up = (float*)((uint8_t*)car + OFF_CAR_MATRIX + 0x20);

    float f[3] = { fwd[0], fwd[1], fwd[2] };
    float r[3] = { right[0], right[1], right[2] };
    float u[3] = { up[0], up[1], up[2] };
    Normalize3(f);
    Normalize3(r);
    Normalize3(u);

    float* pos = (float*)posPtr;
    float* target = (float*)targetPtr;

    // Capture the car-local mount once.  Everything after this is rebuilt from
    // the car transform only, so the game cannot add inertia.
    static float baseOff[3] = { 0.0f, 0.0f, 0.0f };
    static float baseDir[3] = { 1.0f, 0.0f, 0.0f };
    static float baseDist = 10.0f;
    static bool  baseInit = false;
    static void* baseMover = nullptr;
    if (!baseInit || baseMover != mover) {
        float off[3] = { pos[0] - carPos[0], pos[1] - carPos[1], pos[2] - carPos[2] };
        baseOff[0] = Dot3(off, f);
        baseOff[1] = Dot3(off, r);
        baseOff[2] = Dot3(off, u);

        float dir[3] = { target[0] - pos[0], target[1] - pos[1], target[2] - pos[2] };
        baseDist = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (baseDist < 1.0e-3f) baseDist = 10.0f;
        baseDir[0] = Dot3(dir, f);
        baseDir[1] = Dot3(dir, r);
        baseDir[2] = Dot3(dir, u);
        Normalize3(baseDir);
        if (baseDir[0] == 0.0f && baseDir[1] == 0.0f && baseDir[2] == 0.0f) {
            baseDir[0] = 1.0f;
        }
        baseInit = true;
        baseMover = mover;
    }

    float newPos[3] = {
        carPos[0] + f[0] * baseOff[0] + r[0] * baseOff[1] + u[0] * baseOff[2],
        carPos[1] + f[1] * baseOff[0] + r[1] * baseOff[1] + u[1] * baseOff[2],
        carPos[2] + f[2] * baseOff[0] + r[2] * baseOff[1] + u[2] * baseOff[2]
    };
    float dirWorld[3] = {
        f[0] * baseDir[0] + r[0] * baseDir[1] + u[0] * baseDir[2],
        f[1] * baseDir[0] + r[1] * baseDir[1] + u[1] * baseDir[2],
        f[2] * baseDir[0] + r[2] * baseDir[1] + u[2] * baseDir[2]
    };
    Normalize3(dirWorld);

    pos[0] = newPos[0];
    pos[1] = newPos[1];
    pos[2] = newPos[2];
    target[0] = newPos[0] + dirWorld[0] * baseDist;
    target[1] = newPos[1] + dirWorld[1] * baseDist;
    target[2] = newPos[2] + dirWorld[2] * baseDist;

    // HorizonLock=1: level with world up. HorizonLock=0: follow car up, so
    // the camera is fixed tightly to the car's roll.
    float* camUp = (float*)upPtr;
    if (g_horizonLock) {
        camUp[0] = 0.0f;
        camUp[1] = 0.0f;
        camUp[2] = 1.0f;
    } else {
        camUp[0] = u[0];
        camUp[1] = u[1];
        camUp[2] = u[2];
    }
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
static bool WriteCodeProt(void* addr, const void* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, bytes, len);
    DWORD keep = 0;
    VirtualProtect(addr, len, old, &keep);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

static void* AllocNear(uintptr_t target, size_t size) {
    for (uintptr_t base = 0x01000000; base < 0x70000000; base += 0x10000) {
        void* p = VirtualAlloc((LPVOID)base, size,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!p) continue;
        long long diff = (long long)((uintptr_t)p) - (long long)target;
        if (diff > -0x7FF00000LL && diff < 0x7FF00000LL) return p;
        VirtualFree(p, 0, MEM_RELEASE);
    }
    return nullptr;
}

static bool BuildPositionStub(uint8_t* p, uintptr_t helper, uintptr_t original) {
    *p++ = 0x60;                                     // pushad
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x28; // mov eax,[esp+40] (pos)
    *p++ = 0x8B; *p++ = 0x54; *p++ = 0x24; *p++ = 0x2C; // mov edx,[esp+44] (target)
    *p++ = 0x8B; *p++ = 0x5C; *p++ = 0x24; *p++ = 0x30; // mov ebx,[esp+48] (up)
    *p++ = 0x8B; *p++ = 0xCE;                        // mov ecx,esi (mover)
    *p++ = 0x83; *p++ = 0xEC; *p++ = 0x1C;           // sub esp,28
    *p++ = 0xD9; *p++ = 0x34; *p++ = 0x24;           // fnstenv [esp]
    *p++ = 0x53;                                     // push up
    *p++ = 0x50;                                     // push pos
    *p++ = 0x52;                                     // push target
    *p++ = 0x51;                                     // push mover
    *p++ = 0xB8; *(uint32_t*)p = (uint32_t)helper; p += 4; // mov eax,helper
    *p++ = 0xFF; *p++ = 0xD0;                        // call eax
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x10;           // add esp,16
    *p++ = 0xD9; *p++ = 0x2C; *p++ = 0x24;           // fldenv [esp]
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x1C;           // add esp,28
    *p++ = 0x61;                                     // popad
    *p++ = 0xE9;                                     // jmp original
    int32_t rel = (int32_t)(original - ((uintptr_t)p + 4));
    *(int32_t*)p = rel;
    return true;
}

static bool InstallPositionHook() {
    uint8_t* call = (uint8_t*)(g_base + OFF_CALL_POS);
    if (call[0] != 0xE8) {
        LogLine("install: expected position CALL at %p, found %02X", call, call[0]);
        return false;
    }
    int32_t rel = *(int32_t*)(call + 1);
    uintptr_t target = (uintptr_t)(call + 5) + rel;
    uintptr_t expected = g_base + OFF_ORIG_POS;
    if (target != expected) {
        LogLine("install: position call target %p, expected %p", (void*)target, (void*)expected);
        return false;
    }

    void* stub = AllocNear(g_base + OFF_CALL_POS, 0x1000);
    if (!stub) {
        LogLine("install: position stub allocation failed");
        return false;
    }
    memset(stub, 0x90, 0x1000);
    BuildPositionStub((uint8_t*)stub, (uintptr_t)&AdjustCameraPosition, expected);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x1000);

    uint8_t patch[5];
    patch[0] = 0xE8;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)stub - (g_base + OFF_CALL_POS + 5));
    if (!WriteCodeProt(call, patch, 5)) {
        LogLine("install: failed to patch position call at %p", call);
        return false;
    }
    LogLine("install: position hook live. call=%p stub=%p helper=%p", call, stub,
            (void*)&AdjustCameraPosition);
    return true;
}

// ---------------------------------------------------------------------------
// Player car audio centering.  The five calls to the pan update are replaced
// with a stub that calls the original and then forces the shared pan value to
// center when the audio object is player index 0.
// ---------------------------------------------------------------------------
static bool BuildAudioStub(uint8_t* p, uintptr_t original) {
    *p++ = 0x60;                                     // pushad
    *p++ = 0xB8; *(uint32_t*)p = (uint32_t)original; p += 4; // mov eax,original
    *p++ = 0xFF; *p++ = 0xD0;                        // call eax
    *p++ = 0x8B; *p++ = 0x74; *p++ = 0x24; *p++ = 0x18; // mov esi,[esp+24] (saved ECX)
    *p++ = 0x83; *p++ = 0xBE; *(uint32_t*)p = 0xD0; p += 4;
    *p++ = 0x00;                                     // cmp dword [esi+0xD0],0
    *p++ = 0x75; *p++ = 0x2E;                        // jne done (not player)

    *p++ = 0x80; *p++ = 0x3D;
    *(uint32_t*)p = (uint32_t)(uintptr_t)&g_onlyHood; p += 4;
    *p++ = 0x00;                                     // cmp byte [OnlyHood],0
    *p++ = 0x74; *p++ = 0x1C;                        // je center (all cameras)

    *p++ = 0xA1; *(uint32_t*)p = (uint32_t)(g_base + OFF_PLAYER_PTR); p += 4;
    *p++ = 0x85; *p++ = 0xC0;                        // test eax,eax
    *p++ = 0x74; *p++ = 0x1C;                        // je done
    *p++ = 0x8B; *p++ = 0x80; *(uint32_t*)p = OFF_ACTIVE_CAM; p += 4;
    *p++ = 0x85; *p++ = 0xC0;                        // test eax,eax
    *p++ = 0x74; *p++ = 0x12;                        // je done
    *p++ = 0x83; *p++ = 0xB8; *(uint32_t*)p = OFF_POV_TYPE; p += 4;
    *p++ = 0x04;                                     // cmp dword [eax+0x4C],4
    *p++ = 0x75; *p++ = 0x09;                        // jne done (not hood)

    // center: EAXCar pan 0
    *p++ = 0x66; *p++ = 0xC7; *p++ = 0x05;
    *(uint32_t*)p = (uint32_t)(g_base + OFF_AUDIO_PAN_GLOBAL); p += 4;
    *p++ = 0x00; *p++ = 0x00;                        // mov word [pan],0
    // done:
    *p++ = 0x61;                                     // popad
    *p++ = 0xC3;                                     // ret
    return true;
}

static bool InstallAudioHooks() {
    void* stub = AllocNear(g_base + OFF_AUDIO_PAN_CALLS[0], 0x1000);
    if (!stub) {
        LogLine("install: audio stub allocation failed");
        return false;
    }
    memset(stub, 0x90, 0x1000);
    BuildAudioStub((uint8_t*)stub, g_base + OFF_AUDIO_PAN_FN);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x1000);

    int patched = 0;
    for (uint32_t off : OFF_AUDIO_PAN_CALLS) {
        uint8_t* call = (uint8_t*)(g_base + off);
        if (call[0] != 0xE8) {
            LogLine("install: audio call at %p has %02X, skipped", call, call[0]);
            continue;
        }
        int32_t rel = *(int32_t*)(call + 1);
        uintptr_t target = (uintptr_t)(call + 5) + rel;
        if (target != g_base + OFF_AUDIO_PAN_FN) {
            LogLine("install: audio call at %p targets %p, skipped", call, (void*)target);
            continue;
        }
        uint8_t patch[5];
        patch[0] = 0xE8;
        *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)stub - (g_base + off + 5));
        if (WriteCodeProt(call, patch, 5)) patched++;
    }
    LogLine("install: audio center hooks patched %d/%d, stub=%p pan=0",
            patched, (int)(sizeof(OFF_AUDIO_PAN_CALLS) / sizeof(OFF_AUDIO_PAN_CALLS[0])),
            stub);
    return patched > 0;
}

// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_base = (uintptr_t)GetModuleHandleA(nullptr);
        BuildPaths(module);
        ReadConfig();

        BOOL debugLog = GetPrivateProfileIntA("General", "DebugLog", 0, g_iniPath) != 0;
        if (!debugLog) g_logPath[0] = '\0';

        LogLine("=== NFSU2FixedHoodCam attach: base=%08X Enabled=%d OnlyHood=%d ===",
                (uint32_t)g_base, g_enabled ? 1 : 0, g_onlyHood ? 1 : 0);
        LogLine("camera: RemoveMovement=%d HorizonLock=%d",
                g_removeMovement ? 1 : 0, g_horizonLock ? 1 : 0);
        LogLine("audio: CenterPlayer=%d (pan=0, OnlyHood respected)",
                g_centerPlayerAudio ? 1 : 0);

        if (g_base != 0x400000ull) {
            LogLine("install: unexpected module base %08X; aborting", (uint32_t)g_base);
            return TRUE;
        }
        if (g_enabled && !InstallPositionHook()) {
            LogLine("install: camera hook NOT installed");
            return TRUE;
        }
        if (g_enabled && g_centerPlayerAudio && !InstallAudioHooks()) {
            LogLine("install: audio center hook NOT installed");
        }
    }
    return TRUE;
}
