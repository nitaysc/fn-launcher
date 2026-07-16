// main.cpp - Fortnite ESP Box
// Build: v41.10 CL-55227503
#ifndef _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#endif
#include <Windows.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <cmath>
#include <mutex>
#include <atomic>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")

#include <intrin.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")

#include "driver.h"
#include "offsets.h"
#include "vigem.h"

// Forward declarations
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);

// DirectX globals
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static int g_screenWidth = 0;
static int g_screenHeight = 0;

// ============================================================
// GUI Animation state
// ============================================================
static float g_menuAlpha = 1.0f;
static float g_menuTargetAlpha = 1.0f;
static float g_pulsePhase = 0.0f;

// ============================================================
// Types
// ============================================================
struct FVec3 { double x, y, z; };
struct FVec2 { float x, y; };
struct FRotator { double pitch, yaw, roll; };

struct FMatrix {
    double m[4][4];
};

// ============================================================
// Globals
// ============================================================
enum class BoxStyle { CORNER = 0, FILLED, ROUNDED, BOX_3D };
enum class ColorMode { STATIC = 0, DISTANCE_BASED, TEAM_BASED };

struct ESPSettings {
    bool enabled = true;
    bool showBox = true;
    bool showSkeleton = true;
    bool showLocalSkeleton = false;
    bool showDistance = true;
    bool showSnapline = false;
    bool showPlayerName = true;
    bool showHealthBar = true;
    bool showWeaponInfo = true;
    bool showStatusIndicators = true;
    bool showKillCount = false;
    bool showFovCircle = true;
    float fovCircleThickness = 1.5f;

    BoxStyle boxStyle = BoxStyle::CORNER;
    ColorMode colorMode = ColorMode::DISTANCE_BASED;

    float boxColor[4] = { 1.0f, 0.2f, 0.2f, 1.0f };
    float healthColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    float shieldColor[4] = { 0.0f, 0.4f, 1.0f, 1.0f };
    float teamColor[4] = { 0.0f, 1.0f, 1.0f, 1.0f };

    float skeletonLineWidth = 1.5f;
    bool skeletonGradient = true;
    bool skeletonGlow = false;
    bool outlinedText = true;

    float distClose = 50.0f;
    float distMid = 150.0f;
    float distFar = 300.0f;

    int maxDistance = 500;
} g_settings;

// ============================================================
// Aimbot Settings
// ============================================================
struct AimbotSettings {
    bool enabled = false;
    float smooth = 0.12f;      // 0.01=very slow 0.5=fast
    float fov = 15.0f;         // degrees from crosshair
    int aimKey = VK_RBUTTON;   // RMB by default
    bool autoFire = false;
    bool aimAtTeam = false;    // allow aiming at teammates
    bool aimThroughWalls = true;
    bool visibleCheck = false;
    float randomSkip = 0.25f;  // skip ~25% of frames randomly
    float randomOffset = 0.5f; // max degrees of random noise
    float stickSensitivity = 0.85f; // ViGEm right stick sensitivity (0.1-1.0)
    float stickDeadzone = 0.03f;    // deadzone fraction (0.0-0.2)
    int aimToggleKey = 0x50;        // 'P' key to toggle aimbot master switch
    bool masterEnabled = true;      // toggled by aimToggleKey
} g_aim;

struct PlayerData {
    FVec3 position;
    double distance;
    uint8_t teamIndex;
    int killScore;

    bool isKnocked;
    bool isBot;
    bool isCrouched;
    bool isSliding;
    bool isSkydiving;
    bool isInVehicle;

    wchar_t playerName[64];
    wchar_t weaponName[128];
    int ammoCount;
    bool isReloading;

    float health;
    float shield;

    FVec3 bones[16];
    bool hasBones;
};

Driver g_driver;
bool g_driverReady = false;
uint32_t g_targetPID = 0;
uint64_t g_gameBase = 0;
int g_playerCount = 0;
int g_renderFrameIdx = 0;
ViGEmManager g_vigem;

struct ScreenBone { FVec2 s; bool visible; };

struct CachedPlayer {
    PlayerData pd;
    ScreenBone screenBones[16];
    FVec2 screenBase;
    bool screenBaseValid;
    bool valid;
};

struct ESPFrame {
    std::vector<CachedPlayer> players;
    FMatrix viewProj;
    uint8_t localTeam;
    int playerCount;
    bool hasData;
    PlayerData localPlayer;
    ScreenBone localScreenBones[16];
    bool hasLocalPlayer;
};

static ESPFrame g_frames[2];
static std::atomic<int> g_readIdx{0};
static std::mutex g_writeMtx;
static std::atomic<bool> g_espThreadRunning{false};
static std::thread g_espThread;

// ============================================================
// Memory read helpers
// ============================================================
template<typename T>
T Read(uint64_t addr) {
    T val{};
    if (!g_targetPID || !addr) return val;
    xhdr::ProcessRead(g_targetPID, addr, &val, sizeof(T));
    return val;
}

// Batch read: one IOCTL for a large chunk instead of many small ones
inline bool ReadBuffer(uint64_t addr, void* dst, uint32_t size) {
    if (!g_targetPID || !addr || !dst || !size) return false;
    return xhdr::ProcessRead(g_targetPID, addr, dst, size);
}

// ============================================================
// Direct syscall memory write (bypasses EAC's ntdll hooks)
// ============================================================
// Build syscall stubs in executable memory to call NT functions
// without going through EAC-hooked ntdll.

typedef struct _MY_CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} MY_CLIENT_ID;

static HANDLE g_gameHandle = NULL;

// Build a syscall stub: mov r10,rcx; mov eax,SSN; syscall; ret
void* BuildSyscallStub(DWORD syscallNum)
{
    BYTE code[] = {
        0x4C, 0x8B, 0xD1,                         // mov r10, rcx
        0xB8, 0x00, 0x00, 0x00, 0x00,             // mov eax, SSN
        0x0F, 0x05,                                // syscall
        0xC3                                       // ret
    };
    *(DWORD*)(code + 4) = syscallNum;

    void* mem = VirtualAlloc(NULL, sizeof(code), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return NULL;
    memcpy(mem, code, sizeof(code));
    return mem;
}

DWORD GetSyscallNumber(const char* fnName)
{
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) return 0;
    BYTE* fn = (BYTE*)GetProcAddress(hNtdll, fnName);
    if (!fn) return 0;
    if (fn[0] == 0xB8) return *(DWORD*)(fn + 1);
    // Some Windows builds have a jmp to the real implementation
    if (fn[0] == 0xE9) {
        DWORD offset = *(DWORD*)(fn + 1);
        BYTE* target = fn + 5 + offset;
        if (target[0] == 0xB8) return *(DWORD*)(target + 1);
    }
    return 0;
}

bool OpenGameHandle()
{
    if (g_gameHandle) return true;

    DWORD scNtOpenProcess = GetSyscallNumber("NtOpenProcess");
    if (!scNtOpenProcess) {
        static auto pNtOP = (NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, MY_CLIENT_ID*))
            GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtOpenProcess");
        if (!pNtOP) return false;
        MY_CLIENT_ID cid = { (HANDLE)(UINT_PTR)g_targetPID, NULL };
        OBJECT_ATTRIBUTES oa = { sizeof(oa) };
        return pNtOP(&g_gameHandle, PROCESS_VM_WRITE | PROCESS_VM_OPERATION, &oa, &cid) >= 0;
    }

    auto pNtOP = (NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, MY_CLIENT_ID*))BuildSyscallStub(scNtOpenProcess);
    if (!pNtOP) return false;
    MY_CLIENT_ID cid = { (HANDLE)(UINT_PTR)g_targetPID, NULL };
    OBJECT_ATTRIBUTES oa = { sizeof(oa) };
    NTSTATUS sts = pNtOP(&g_gameHandle, PROCESS_VM_WRITE | PROCESS_VM_OPERATION, &oa, &cid);
    VirtualFree(pNtOP, 0, MEM_RELEASE);
    if (sts < 0 || !g_gameHandle) return false;
    return true;
}

template<typename T>
void WriteSafe(uint64_t addr, const T& val)
{
    if (!g_targetPID || !addr) return;
    if (!g_gameHandle && !OpenGameHandle()) return;

    DWORD scNtWriteVM = GetSyscallNumber("NtWriteVirtualMemory");
    if (!scNtWriteVM) return;

    auto pNtWriteVM = (NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, ULONG, PULONG))BuildSyscallStub(scNtWriteVM);
    if (!pNtWriteVM) return;

    ULONG written = 0;
    pNtWriteVM(g_gameHandle, (PVOID)addr, (PVOID)&val, sizeof(T), &written);
    VirtualFree(pNtWriteVM, 0, MEM_RELEASE);
}

// ============================================================
// Probe undocumented IOCTL commands in the kernel driver
// Tries writing a test value to a known address using different
// command codes (since the driver has no official write command).
// ============================================================
void ProbeDriverWrite(uint64_t testAddr, const void* testData, uint32_t dataSize)
{
    struct {
        UINT64 handle;
        UINT64 dstVA;    // target address
        UINT64 srcPtr;   // source data (in our process space)
        DWORD  size;
    } p;

    HANDLE h = xhdr::GetTargetHandle(g_targetPID);
    if (!h) {
        printf("[PROBE] No target handle\n");
        return;
    }

    // Use the cached handle directly
    p.handle = (UINT64)h;
    p.dstVA  = testAddr;
    p.srcPtr = (UINT64)testData;
    p.size   = dataSize;

    const DWORD codesToTry[] = {
        778, 779, 780, 782, 783, 784, 786, 789, 790, 791, 792,
        793, 794, 795, 796, 797, 798, 799, 800, 802, 803, 804,
        805, 806, 807, 808, 809, 810, 811, 812, 813, 814, 816, 817, 818
    };

    for (DWORD code : codesToTry) {
        xhdr::SendCmd(code, &p, sizeof(p), true);
        DWORD sts = xhdr::GetThreadRsp().rsp.status;
        if ((INT32)sts >= 0) {
            printf("[PROBE] CMD %u returned STATUS=0x%08X (SUCCESS!)\n", code, (unsigned)sts);
        } else if (sts != 0xC0000001 && sts != 0xC0000010 && sts != 0xC00000BB) {
            // Log unexpected errors (not the common "unsupported" ones)
            printf("[PROBE] CMD %u -> 0x%08X\n", code, (unsigned)sts);
        }
    }
    printf("[PROBE] Done probing\n");
}

// Write via xhunter1 cmd 786 (byte-buffer approach, no alignment issues)
template<typename T>
void Write786(uint64_t addr, const T& val) {
    if (!g_targetPID || !addr) return;
    BYTE buf[128] = {};
    *(UINT64*)(buf + 0) = (UINT64)xhdr::GetTargetHandle(g_targetPID);
    *(UINT64*)(buf + 8) = addr;
    *(UINT32*)(buf + 16) = (UINT32)sizeof(T);
    memcpy(buf + 20, &val, sizeof(T));
    DWORD totalSize = 20 + (DWORD)sizeof(T);
    xhdr::SendCmd(786, buf, totalSize, true);
}

// Async write through driver queue (returns immediately, worker drains)
template<typename T>
void Write(uint64_t addr, const T& val) {
    if (!g_targetPID || !addr) return;
    xhdr::ProcessWrite(g_targetPID, addr, &val, sizeof(T));
}

// ============================================================
// Get UWorld (decrypt from static address)
// ============================================================
uint64_t GetUWorld()
{
    if (!g_targetPID || !g_gameBase) return 0;
    uint64_t encrypted = Read<uint64_t>(g_gameBase + offsets::core::UWORLD);
    return offsets::uworld::decrypt(encrypted);
}

void DebugUWorld()
{
    if (!g_targetPID || !g_gameBase) {
        printf("[-] No target\n"); return;
    }
    printf("[DBG] gameBase=0x%llX\n", (unsigned long long)g_gameBase);
    printf("[DBG] reading UWorld from: 0x%llX\n", (unsigned long long)(g_gameBase + offsets::core::UWORLD));
    
    uint64_t encrypted = Read<uint64_t>(g_gameBase + offsets::core::UWORLD);
    printf("[DBG] encrypted=0x%llX\n", (unsigned long long)encrypted);
    
    uint64_t decrypted = offsets::uworld::decrypt(encrypted);
    printf("[DBG] decrypted=0x%llX\n", (unsigned long long)decrypted);
    
    if (decrypted) {
        uint64_t level = Read<uint64_t>(decrypted + offsets::core::PersistentLevel);
        uint64_t gs = Read<uint64_t>(decrypted + offsets::core::GameState);
        printf("[DBG] PersistentLevel=0x%llX GameState=0x%llX\n",
            (unsigned long long)level, (unsigned long long)gs);
    }
}

// ============================================================
// WorldToScreen using ViewProjectionMatrix (proper method)
// ============================================================
static FMatrix g_viewProjectionMatrix = {};

bool WorldToScreen(FVec3 world, FVec2& screen)
{
    // Multiply world position by ViewProjectionMatrix
    double x = world.x * g_viewProjectionMatrix.m[0][0] +
               world.y * g_viewProjectionMatrix.m[1][0] +
               world.z * g_viewProjectionMatrix.m[2][0] +
                          g_viewProjectionMatrix.m[3][0];

    double y = world.x * g_viewProjectionMatrix.m[0][1] +
               world.y * g_viewProjectionMatrix.m[1][1] +
               world.z * g_viewProjectionMatrix.m[2][1] +
                          g_viewProjectionMatrix.m[3][1];

    double w = world.x * g_viewProjectionMatrix.m[0][3] +
               world.y * g_viewProjectionMatrix.m[1][3] +
               world.z * g_viewProjectionMatrix.m[2][3] +
                          g_viewProjectionMatrix.m[3][3];

    if (w <= 0.0) return false;

    double rhw = 1.0 / w;
    screen.x = (float)((x * rhw + 1.0) * 0.5 * g_screenWidth);
    screen.y = (float)((1.0 - y * rhw) * 0.5 * g_screenHeight);

    return true;
}

// ============================================================
// Bone indices for Fortnite UE5 skeleton
// ============================================================
enum BoneID {
    BONE_HEAD       = 110,
    BONE_NECK       = 66,
    BONE_CHEST      = 37,
    BONE_PELVIS     = 2,
    BONE_L_SHOULDER = 9,
    BONE_L_ELBOW    = 10,
    BONE_L_HAND     = 11,
    BONE_R_SHOULDER = 38,
    BONE_R_ELBOW    = 39,
    BONE_R_HAND     = 40,
    BONE_L_THIGH    = 71,
    BONE_L_KNEE     = 72,
    BONE_L_FOOT     = 75,
    BONE_R_THIGH    = 78,
    BONE_R_KNEE     = 79,
    BONE_R_FOOT     = 82,
};

// Quaternion for rotation
struct FQuat { double x, y, z, w; };

// Rotate a vector by a quaternion
FVec3 RotateByQuat(FVec3 v, FQuat q)
{
    // q * v * q^-1 (optimized formula)
    double t2x = q.x * 2.0;
    double t2y = q.y * 2.0;
    double t2z = q.z * 2.0;
    double t2xw = t2x * q.w;
    double t2yw = t2y * q.w;
    double t2zw = t2z * q.w;
    double t2xx = t2x * q.x;
    double t2xy = t2x * q.y;
    double t2xz = t2x * q.z;
    double t2yy = t2y * q.y;
    double t2yz = t2y * q.z;
    double t2zz = t2z * q.z;

    FVec3 result;
    result.x = (1.0 - (t2yy + t2zz)) * v.x + (t2xy - t2zw) * v.y + (t2xz + t2yw) * v.z;
    result.y = (t2xy + t2zw) * v.x + (1.0 - (t2xx + t2zz)) * v.y + (t2yz - t2xw) * v.z;
    result.z = (t2xz - t2yw) * v.x + (t2yz + t2xw) * v.y + (1.0 - (t2xx + t2yy)) * v.z;
    return result;
}

// Read a bone's world position from the skeletal mesh component
// Batch version: reads ComponentToWorld once and caches bone array pointer
struct MeshCache {
    FQuat rot;
    FVec3 pos;
    uint64_t boneArray;
    bool valid;
};

MeshCache GetMeshCache(uint64_t mesh)
{
    MeshCache c = {};
    if (!mesh) return c;
    // Read everything in one IOCTL (ComponentToWorld + BoneArray_cache)
    uint8_t buf[0x668];
    if (!ReadBuffer(mesh, buf, sizeof(buf))) return c;
    c.rot = *(FQuat*)(buf + offsets::core::ComponentToWorld);
    c.pos = *(FVec3*)(buf + offsets::core::ComponentToWorld + 0x20);
    c.boneArray = *(uint64_t*)(buf + offsets::core::BoneArray_cache);
    if (!c.boneArray)
        c.boneArray = *(uint64_t*)(buf + offsets::core::BoneArray);
    c.valid = (c.boneArray != 0);
    return c;
}

FVec3 GetBonePosFromCache(const MeshCache& mc, int boneIdx)
{
    FVec3 result = {};
    if (!mc.valid) return result;
    FVec3 boneLocal = Read<FVec3>(mc.boneArray + (boneIdx * 0x60) + 0x20);
    FVec3 rotated = RotateByQuat(boneLocal, mc.rot);
    result.x = rotated.x + mc.pos.x;
    result.y = rotated.y + mc.pos.y;
    result.z = rotated.z + mc.pos.z;
    return result;
}

// ============================================================
// Outlined text helper
// ============================================================
void DrawText(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, bool centered = false)
{
    if (centered) {
        ImVec2 sz = ImGui::CalcTextSize(text);
        pos.x -= sz.x * 0.5f;
    }
    ImU32 bg = IM_COL32(0, 0, 0, 180);
    dl->AddText(ImVec2(pos.x + 1, pos.y + 1), bg, text);
    dl->AddText(pos, col, text);
}

// ============================================================
// Color helpers
// ============================================================
static ImU32 LerpColor(ImU32 a, ImU32 b, float t)
{
    int ra = (a >> IM_COL32_R_SHIFT) & 0xFF, ga = (a >> IM_COL32_G_SHIFT) & 0xFF, ba = (a >> IM_COL32_B_SHIFT) & 0xFF;
    int rb = (b >> IM_COL32_R_SHIFT) & 0xFF, gb = (b >> IM_COL32_G_SHIFT) & 0xFF, bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
    return IM_COL32((int)(ra + (rb - ra) * t), (int)(ga + (gb - ga) * t), (int)(ba + (bb - ba) * t), 255);
}

ImU32 GetDistanceColor(double dist)
{
    ImU32 red = IM_COL32(255, 50, 50, 255);
    ImU32 yellow = IM_COL32(255, 255, 50, 255);
    ImU32 green = IM_COL32(50, 255, 50, 255);

    if (dist <= g_settings.distClose) return red;
    if (dist >= g_settings.distFar)   return green;
    if (dist < g_settings.distMid) {
        float t = (float)(dist - g_settings.distClose) / (g_settings.distMid - g_settings.distClose);
        return LerpColor(red, yellow, t);
    }
    float t = (float)(dist - g_settings.distMid) / (g_settings.distFar - g_settings.distMid);
    return LerpColor(yellow, green, t);
}

ImU32 GetPlayerColor(const PlayerData& pd, uint8_t localTeam)
{
    if (g_settings.colorMode == ColorMode::TEAM_BASED && pd.teamIndex == localTeam && localTeam != 0)
        return ImGui::ColorConvertFloat4ToU32(ImVec4(g_settings.teamColor[0], g_settings.teamColor[1], g_settings.teamColor[2], g_settings.teamColor[3]));
    if (g_settings.colorMode == ColorMode::DISTANCE_BASED)
        return GetDistanceColor(pd.distance);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(g_settings.boxColor[0], g_settings.boxColor[1], g_settings.boxColor[2], g_settings.boxColor[3]));
}

// ============================================================
// Box renderers
// ============================================================
void DrawCornerBox(ImDrawList* dl, float x1, float y1, float x2, float y2, ImU32 col)
{
    float cw = x2 - x1, ch = y2 - y1;
    float cl = (cw < ch ? cw : ch) * 0.25f;
    ImU32 bg = IM_COL32(0, 0, 0, 180);
    auto C = [&](ImVec2 a, ImVec2 b) { dl->AddLine(a, b, bg, 3.f); dl->AddLine(a, b, col, 2.f); };
    C({x1,y1},{x1+cl,y1}); C({x1,y1},{x1,y1+cl});
    C({x2,y1},{x2-cl,y1}); C({x2,y1},{x2,y1+cl});
    C({x1,y2},{x1+cl,y2}); C({x1,y2},{x1,y2-cl});
    C({x2,y2},{x2-cl,y2}); C({x2,y2},{x2,y2-cl});
}

void DrawFilledBox(ImDrawList* dl, float x1, float y1, float x2, float y2, ImU32 col)
{
    ImVec4 cv = ImGui::ColorConvertU32ToFloat4(col);
    ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(cv.x, cv.y, cv.z, 0.12f));
    dl->AddRectFilled({x1,y1}, {x2,y2}, fill);
    dl->AddRect({x1,y1}, {x2,y2}, col, 0.f, 0, 1.5f);
}

void DrawRoundedBox(ImDrawList* dl, float x1, float y1, float x2, float y2, ImU32 col)
{
    ImVec4 cv = ImGui::ColorConvertU32ToFloat4(col);
    ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(cv.x, cv.y, cv.z, 0.08f));
    dl->AddRectFilled({x1,y1}, {x2,y2}, fill, 5.f);
    dl->AddRect({x1,y1}, {x2,y2}, col, 5.f, 0, 1.5f);
}

void Draw3DBox(ImDrawList* dl, const PlayerData& pd, ImU32 col)
{
    if (!pd.hasBones) return;
    FVec3 c = pd.position;
    double hw = 30.0;
    if (pd.bones[4].x != 0.0 || pd.bones[7].x != 0.0) {
        double sx = pd.bones[7].x - pd.bones[4].x;
        double sy = pd.bones[7].y - pd.bones[4].y;
        hw = sqrt(sx * sx + sy * sy) * 0.6;
        if (hw < 15.0) hw = 15.0;
    }
    double hh = 90.0;
    if (pd.bones[0].z != 0.0 && pd.bones[3].z != 0.0)
        hh = fabs(pd.bones[0].z - pd.bones[3].z) * 0.6;

    FVec3 corners[8] = {
        {c.x-hw, c.y-hw, c.z-hh}, {c.x+hw, c.y-hw, c.z-hh},
        {c.x+hw, c.y+hw, c.z-hh}, {c.x-hw, c.y+hw, c.z-hh},
        {c.x-hw, c.y-hw, c.z+hh}, {c.x+hw, c.y-hw, c.z+hh},
        {c.x+hw, c.y+hw, c.z+hh}, {c.x-hw, c.y+hw, c.z+hh},
    };
    FVec2 sc[8];
    bool ok[8];
    for (int i = 0; i < 8; i++) ok[i] = WorldToScreen(corners[i], sc[i]);

    int edges[][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (auto& e : edges) {
        if (ok[e[0]] && ok[e[1]])
            dl->AddLine({sc[e[0]].x, sc[e[0]].y}, {sc[e[1]].x, sc[e[1]].y}, col, 1.2f);
    }
}

// ============================================================
// Health/Shield bar renderer
// ============================================================
void DrawHealthBars(ImDrawList* dl, float x, float y1, float y2, float hp, float shield)
{
    float barW = 4.0f;
    float h = y2 - y1;
    ImU32 bg = IM_COL32(0, 0, 0, 160);
    ImU32 outline = IM_COL32(0, 0, 0, 255);

    float hpPct = hp / 100.0f;
    if (hpPct > 1.0f) hpPct = 1.0f;
    if (hpPct < 0.0f) hpPct = 0.0f;
    float hpH = h * hpPct;
    ImU32 hpCol;
    if (hpPct > 0.5f) hpCol = IM_COL32(50, 255, 50, 255);
    else if (hpPct > 0.25f) hpCol = IM_COL32(255, 200, 50, 255);
    else hpCol = IM_COL32(255, 50, 50, 255);

    dl->AddRectFilled({x, y1}, {x + barW, y2}, bg);
    dl->AddRectFilled({x, y2 - hpH}, {x + barW, y2}, hpCol);
    dl->AddRect({x, y1}, {x + barW, y2}, outline, 0.f, 0, 1.f);

    float sx = x + barW + 2.0f;
    float shPct = shield / 100.0f;
    if (shPct > 1.0f) shPct = 1.0f;
    if (shPct < 0.0f) shPct = 0.0f;
    if (shPct > 0.0f) {
        float shH = h * shPct;
        ImU32 shCol = IM_COL32(50, 120, 255, 255);
        dl->AddRectFilled({sx, y1}, {sx + barW, y2}, bg);
        dl->AddRectFilled({sx, y2 - shH}, {sx + barW, y2}, shCol);
        dl->AddRect({sx, y1}, {sx + barW, y2}, outline, 0.f, 0, 1.f);
    }
}

// ============================================================
// Skeleton renderer with gradient + glow
// ============================================================
void DrawSkeleton(ImDrawList* dl, const ScreenBone* screenBones, bool hasBones, ImU32 baseCol)
{
    if (!hasBones) return;
    struct Conn { int a, b; };
    Conn conns[] = {
        {0,1},{1,2},{2,3},{1,4},{4,5},{5,6},{1,7},{7,8},{8,9},
        {3,10},{10,11},{11,12},{3,13},{13,14},{14,15}
    };
    for (auto& cn : conns) {
        const ScreenBone& sa = screenBones[cn.a];
        const ScreenBone& sb = screenBones[cn.b];
        if (!sa.visible || !sb.visible) continue;

        ImU32 lineCol = baseCol;
        if (g_settings.skeletonGradient) {
            float t = (float)cn.a / 15.0f;
            int r = (int)(255 * (1.0f - t * 0.5f));
            int g = (int)(180 * (1.0f - fabs(t - 0.5f) * 2.0f) + 75);
            int bv = (int)(255 * t);
            lineCol = IM_COL32(r, g, bv, 255);
        }
        if (g_settings.skeletonGlow)
            dl->AddLine({sa.s.x,sa.s.y}, {sb.s.x,sb.s.y}, IM_COL32(0,0,0,120), g_settings.skeletonLineWidth + 2.5f);
        dl->AddLine({sa.s.x,sa.s.y}, {sb.s.x,sb.s.y}, lineCol, g_settings.skeletonLineWidth);
    }
}

// ============================================================
// FString reading (FString = TArray<wchar_t>: ptr[0x0], num[0x8], max[0xC])
// ============================================================
bool ReadFString(uint64_t addr, wchar_t* out, int maxChars)
{
    out[0] = L'\0';
    if (!addr) return false;
    uint64_t dataPtr = Read<uint64_t>(addr);
    if (!dataPtr || dataPtr < 0x10000) return false;
    int32_t len = Read<int32_t>(addr + 0x8);
    if (len <= 0 || len > maxChars) return false;
    xhdr::ProcessRead(g_targetPID, dataPtr, out, len * sizeof(wchar_t));
    out[len - 1] = L'\0';
    return out[0] != L'\0';
}

bool ReadFText(uint64_t addr, wchar_t* out, int maxChars)
{
    out[0] = L'\0';
    if (!addr) return false;
    uint64_t sharedRef = Read<uint64_t>(addr);
    if (!sharedRef || sharedRef < 0x10000) return false;
    return ReadFString(sharedRef + 0x28, out, maxChars);
}

// ============================================================
// Read all player data (batched IOCTL version)
// ============================================================
PlayerData ReadPlayerDataFor(uint64_t playerState, uint64_t pawn, FVec3 localPos, FVec3 prePos, MeshCache& mc)
{
    PlayerData pd = {};
    pd.health = 100.f;
    pd.shield = 0.f;
    pd.playerName[0] = L'\0';
    pd.position = prePos;

    double dx = pd.position.x - localPos.x;
    double dy = pd.position.y - localPos.y;
    double dz = pd.position.z - localPos.z;
    pd.distance = sqrt(dx * dx + dy * dy + dz * dz) / 100.0;

    // Batch: read entire pawn block in ONE IOCTL
    uint8_t pawnBuf[0x2178];
    if (!ReadBuffer(pawn, pawnBuf, sizeof(pawnBuf)))
        return pd;

    // Batch: read player state
    uint8_t psBuf[0xF30 + 8];
    if (ReadBuffer(playerState, psBuf, sizeof(psBuf))) {
        pd.teamIndex = psBuf[offsets::player::TeamIndex];
        pd.killScore = *(int32_t*)(psBuf + offsets::player::KillScore);
        pd.isBot = (psBuf[offsets::player::bIsABot] & 0x8) != 0;
    }

    pd.isKnocked   = (pawnBuf[offsets::player::bIsDBNO] & 0x80) != 0;
    pd.isCrouched  = (pawnBuf[offsets::player::bIsCrouched] & 0x2) != 0;
    pd.isSliding   = (pawnBuf[offsets::player::bIsSliding] & 0x10) != 0;
    pd.isSkydiving = (pawnBuf[offsets::player::bIsSkydiving] & 0x1) != 0;
    pd.isInVehicle = (pawnBuf[offsets::player::bIsDying] & 0x20) != 0;

    uint64_t weapon = *(uint64_t*)(pawnBuf + offsets::player::CurrentWeapon);
    if (weapon) {
        uint8_t wBuf[0x10B0];
        if (ReadBuffer(weapon, wBuf, sizeof(wBuf))) {
            uint64_t wData = *(uint64_t*)(wBuf + offsets::weapon::WeaponData);
            if (wData) ReadFText(wData + offsets::weapon::ItemName, pd.weaponName, 128);
            pd.ammoCount = *(int32_t*)(wBuf + offsets::weapon::AmmoCount);
            pd.isReloading = (wBuf[offsets::weapon::bIsReloading] & 0x1) != 0;
        }
    }

    uint64_t mesh = *(uint64_t*)(pawnBuf + offsets::player::Mesh);
    mc = GetMeshCache(mesh);
    pd.hasBones = mc.valid;
    if (mc.valid) {
        int ids[16] = { BONE_HEAD, BONE_NECK, BONE_CHEST, BONE_PELVIS,
            BONE_L_SHOULDER, BONE_L_ELBOW, BONE_L_HAND,
            BONE_R_SHOULDER, BONE_R_ELBOW, BONE_R_HAND,
            BONE_L_THIGH, BONE_L_KNEE, BONE_L_FOOT,
            BONE_R_THIGH, BONE_R_KNEE, BONE_R_FOOT };

        uint8_t boneBuf[0x3000];
        if (ReadBuffer(mc.boneArray, boneBuf, sizeof(boneBuf))) {
            for (int j = 0; j < 16; j++) {
                uint32_t off = ids[j] * 0x60 + 0x20;
                if (off + 24 > sizeof(boneBuf)) continue;
                FVec3 boneLocal = *(FVec3*)(boneBuf + off);
                FVec3 rotated = RotateByQuat(boneLocal, mc.rot);
                pd.bones[j].x = rotated.x + mc.pos.x;
                pd.bones[j].y = rotated.y + mc.pos.y;
                pd.bones[j].z = rotated.z + mc.pos.z;
            }
        }
        pd.hasBones = (pd.bones[0].x != 0.0 || pd.bones[0].y != 0.0 || pd.bones[0].z != 0.0);
    }
    return pd;
}

// ============================================================
// Aimbot
// ============================================================
#define M_PI 3.14159265358979323846

FVec3 GetCameraLocation()
{
    uint64_t uworld = GetUWorld();
    if (!uworld) return {};
    uint64_t viewArrayData = Read<uint64_t>(uworld + offsets::core::CachedViewInfoRenderedLastFrame);
    if (!viewArrayData) return {};

    static int dbgC = 0;
    bool dbgP = (++dbgC % 240) == 0; // print every ~4 seconds
    if (dbgC > 100000) dbgC = 0;

    uint32_t locOffsets[] = { 0, 0x40, 0x60, 0x80, 0xA0, 0x100 };
    for (auto off : locOffsets) {
        FVec3 loc = Read<FVec3>(viewArrayData + off);
        if (fabs(loc.x) > 1.0 && fabs(loc.x) < 1000000.0 &&
            fabs(loc.y) > 1.0 && fabs(loc.y) < 1000000.0 &&
            fabs(loc.z) > 1.0 && fabs(loc.z) < 1000000.0) {
            if (dbgP) printf("[CAM] offset 0x%X: (%.0f,%.0f,%.0f)\n",
                off, loc.x, loc.y, loc.z);
            return loc;
        }
    }
    return {};
}

FRotator CalculateAimAngles(FVec3 cameraPos, FVec3 targetPos, FRotator currentRot)
{
    FVec3 delta;
    delta.x = targetPos.x - cameraPos.x;
    delta.y = targetPos.y - cameraPos.y;
    delta.z = targetPos.z - cameraPos.z;

    double dist = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (dist < 1.0) return currentRot;

    double pitch = -asin(delta.z / dist) * (180.0 / M_PI);
    double yaw   = atan2(delta.y, delta.x) * (180.0 / M_PI);

    if (pitch > 89.0)  pitch = 89.0;
    if (pitch < -89.0) pitch = -89.0;
    if (yaw < 0.0)     yaw += 360.0;

    return { pitch, yaw, 0.0 };
}

FRotator SmoothRot(FRotator current, FRotator target, float smoothFactor)
{
    double dp = target.pitch - current.pitch;
    double dy = target.yaw - current.yaw;

    while (dy > 180.0)  dy -= 360.0;
    while (dy < -180.0) dy += 360.0;

    current.pitch += dp * smoothFactor;
    current.yaw   += dy * smoothFactor;

    if (current.pitch > 89.0)  current.pitch = 89.0;
    if (current.pitch < -89.0) current.pitch = -89.0;
    if (current.yaw < 0.0)     current.yaw += 360.0;
    if (current.yaw >= 360.0)  current.yaw -= 360.0;

    return current;
}

float ScreenDistToCrosshair(FVec2 screenPos)
{
    float cx = (float)g_screenWidth * 0.5f;
    float cy = (float)g_screenHeight * 0.5f;
    float dx = screenPos.x - cx;
    float dy = screenPos.y - cy;
    return sqrtf(dx * dx + dy * dy);
}

void RunAimbot()
{
    if (!g_aim.enabled || !g_aim.masterEnabled || !g_driverReady || !g_targetPID) return;
    if (!g_vigem.IsReady()) return;

    // Toggle master switch with 'P'
    static bool togglePrev = false;
    bool toggleNow = (GetAsyncKeyState(g_aim.aimToggleKey) & 1) != 0;
    if (toggleNow && !togglePrev) g_aim.masterEnabled = !g_aim.masterEnabled;
    togglePrev = toggleNow;
    if (!g_aim.masterEnabled) {
        XUSB_REPORT report = {};
        g_vigem.Update(report);
        return;
    }

    static bool keyHeld = false;
    bool keyDown = (GetAsyncKeyState(g_aim.aimKey) & 0x8000) != 0;
    if (!keyDown) {
        if (keyHeld) {
            XUSB_REPORT report = {};
            g_vigem.Update(report);
            keyHeld = false;
        }
        return;
    }
    if (!keyHeld) {
        keyHeld = true;
    }

    const ESPFrame& frame = g_frames[g_renderFrameIdx];
    if (!frame.hasData) return;

    if ((rand() % 1000) < (int)(g_aim.randomSkip * 1000.0f)) return;

    // FOV radius in pixels (matches the visual FOV circle)
    float fovRadius = g_aim.fov * (g_screenWidth / 90.0f);
    float bestDist = fovRadius;
    int bestIdx = -1;
    FVec2 bestScreen = {};

    // Target stickiness: prefer current target unless another is 40% closer
    static int lockedIdx = -1;
    if (lockedIdx >= (int)frame.players.size()) lockedIdx = -1;

    for (size_t i = 0; i < frame.players.size(); i++) {
        const CachedPlayer& cp = frame.players[i];
        if (!cp.valid) continue;
        if (!g_aim.aimAtTeam && cp.pd.teamIndex == frame.localTeam && frame.localTeam != 0) continue;

        FVec3 targetPos;
        bool hasPos = false;
        if (cp.pd.hasBones) {
            targetPos = cp.pd.bones[offsets::aimbot::BONE_HEAD];
            // At longer ranges, aim slightly above the head to center on hitbox
            if (cp.pd.distance > 150.0) {
                targetPos.z += (cp.pd.distance - 150.0) * 0.15;
                if (targetPos.z - cp.pd.bones[offsets::aimbot::BONE_HEAD].z > 60.0)
                    targetPos.z = cp.pd.bones[offsets::aimbot::BONE_HEAD].z + 60.0;
            }
            hasPos = (targetPos.x != 0.0 || targetPos.y != 0.0 || targetPos.z != 0.0);
        }
        if (!hasPos) {
            if (cp.pd.hasBones) {
                targetPos = cp.pd.bones[offsets::aimbot::BONE_CHEST];
                hasPos = (targetPos.x != 0.0 || targetPos.y != 0.0 || targetPos.z != 0.0);
            }
        }
        if (!hasPos) {
            targetPos = cp.pd.position;
            hasPos = (targetPos.x != 0.0 || targetPos.y != 0.0 || targetPos.z != 0.0);
        }
        if (!hasPos) continue;

        FVec2 screen;
        if (!WorldToScreen(targetPos, screen)) continue;

        float screenDist = ScreenDistToCrosshair(screen);
        // Bias toward locked target (40% advantage = new target must be 40% closer to steal)
        if ((int)i == lockedIdx) screenDist *= 0.6f;
        if (screenDist < bestDist) {
            bestDist = screenDist;
            bestIdx = (int)i;
            bestScreen = screen;
        }
    }

    if (bestIdx < 0) { lockedIdx = -1; return; }
    lockedIdx = bestIdx;

    // Proportional control: smooth approach, no oscillation
    float cx = g_screenWidth * 0.5f;
    float cy = g_screenHeight * 0.5f;
    float dx = bestScreen.x - cx;
    float dy = bestScreen.y - cy;
    float pixelDist = sqrtf(dx * dx + dy * dy);

    // Smooth slider controls aggression (0.01 = OP snap, 0.50 = gentle smooth)
    float deadzonePx = 0.5f + g_aim.smooth * 5.0f;  // 0.55px (OP) .. 3.0px (smooth)
    if (pixelDist < deadzonePx) {
        XUSB_REPORT report = {};
        g_vigem.Update(report);
        return;
    }

    // Power curve exponent: 0.15 (OP snap) .. 1.0 (linear smooth)
    float exponent = 0.15f + g_aim.smooth * 1.7f;
    float t = (pixelDist - deadzonePx) / (100.0f - deadzonePx);
    if (t > 1.0f) t = 1.0f;
    float deflection = pow(t, exponent) * g_aim.stickSensitivity;

    // Cap close-range force to prevent oscillation (max 40% within 10px)
    if (pixelDist < 10.0f && deflection > 0.40f)
        deflection = 0.40f;

    float nx = (dx / pixelDist) * deflection;
    float ny = (dy / pixelDist) * deflection;

    if (nx > 1.0f) nx = 1.0f; if (nx < -1.0f) nx = -1.0f;
    if (ny > 1.0f) ny = 1.0f; if (ny < -1.0f) ny = -1.0f;

    XUSB_REPORT report = {};
    report.sThumbRX = (SHORT)(nx * 32767);
    report.sThumbRY = (SHORT)(-ny * 32767);
    g_vigem.Update(report);
}
void CollectESPData(ESPFrame& frame)
{
    frame.hasData = false;
    frame.players.clear();
    frame.playerCount = 0;
    frame.hasLocalPlayer = false;

    if (!g_settings.enabled || !g_driverReady || !g_targetPID) return;

    uint64_t uworld = GetUWorld();
    if (!uworld) return;

    uint64_t gameState = Read<uint64_t>(uworld + offsets::core::GameState);
    if (!gameState) return;

    uint64_t playerArrayData = Read<uint64_t>(gameState + offsets::core::PlayerArray);
    int32_t playerCount = Read<int32_t>(gameState + offsets::core::PlayerArray + 0x8);
    if (!playerArrayData || playerCount <= 0 || playerCount > 200) return;
    frame.playerCount = playerCount;

    uint64_t gameInstance = Read<uint64_t>(uworld + offsets::core::GameInstance);
    if (!gameInstance) return;
    uint64_t localPlayersArr = Read<uint64_t>(gameInstance + offsets::player::LocalPlayers);
    if (!localPlayersArr) return;
    uint64_t localPlayer = Read<uint64_t>(localPlayersArr);
    if (!localPlayer) return;
    uint64_t playerController = Read<uint64_t>(localPlayer + offsets::player::PlayerController);
    if (!playerController) return;
    uint64_t localPawn = Read<uint64_t>(playerController + offsets::player::LocalPawn);

    uint64_t viewArrayData = Read<uint64_t>(uworld + offsets::core::CachedViewInfoRenderedLastFrame);
    int32_t viewArrayCount = Read<int32_t>(uworld + offsets::core::CachedViewInfoRenderedLastFrame + 0x8);
    if (!viewArrayData || viewArrayCount <= 0) return;
    frame.viewProj = Read<FMatrix>(viewArrayData + 256);
    g_viewProjectionMatrix = frame.viewProj;

    FVec3 localPos = {};
    frame.localTeam = 0;
    if (localPawn) {
        uint64_t localRoot = Read<uint64_t>(localPawn + offsets::core::RootComponent);
        if (localRoot) localPos = Read<FVec3>(localRoot + offsets::core::RelativeLocation);
        uint64_t lps = Read<uint64_t>(localPawn + offsets::player::PlayerState);
        if (lps) frame.localTeam = Read<uint8_t>(lps + offsets::player::TeamIndex);

        if (g_settings.showLocalSkeleton) {
            uint64_t localMesh = Read<uint64_t>(localPawn + offsets::player::Mesh);
            if (localMesh) {
                MeshCache localMC = GetMeshCache(localMesh);
                
                int ids[16] = { BONE_HEAD, BONE_NECK, BONE_CHEST, BONE_PELVIS,
                    BONE_L_SHOULDER, BONE_L_ELBOW, BONE_L_HAND,
                    BONE_R_SHOULDER, BONE_R_ELBOW, BONE_R_HAND,
                    BONE_L_THIGH, BONE_L_KNEE, BONE_L_FOOT,
                    BONE_R_THIGH, BONE_R_KNEE, BONE_R_FOOT };
                for (int j = 0; j < 16; j++) {
                    frame.localPlayer.bones[j] = GetBonePosFromCache(localMC, ids[j]);
                    frame.localScreenBones[j].visible = WorldToScreen(frame.localPlayer.bones[j], frame.localScreenBones[j].s);
                }
                frame.localPlayer.hasBones = true;
                frame.localPlayer.position = localPos;
                frame.hasLocalPlayer = true;
            }
        }
    }

    int limit = playerCount < 100 ? playerCount : 100;
    frame.players.reserve(limit);

    for (int i = 0; i < limit; i++) {
        uint64_t playerState = Read<uint64_t>(playerArrayData + i * 8);
        if (!playerState) continue;

        uint64_t pawn = Read<uint64_t>(playerState + offsets::player::PawnPrivate);
        if (!pawn || pawn == localPawn) continue;

        uint8_t pawnFlags = Read<uint8_t>(pawn + offsets::player::bIsDying);
        // bit 0-2 = dead/eliminated, bit 5 = in vehicle
        if (pawnFlags & 0x07) continue;

        // Quick position check (1 IOCTL: read rootComp + position)
        FVec3 pos{};
        {
            uint64_t rc = Read<uint64_t>(pawn + offsets::core::RootComponent);
            if (!rc) continue;
            ReadBuffer(rc + offsets::core::RelativeLocation, &pos, 24);
        }
        double dx = pos.x - localPos.x, dy = pos.y - localPos.y, dz = pos.z - localPos.z;
        double dist = sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
        if (dist > g_settings.maxDistance || dist < 1.0) continue;

        MeshCache mc = {};
        PlayerData pd = ReadPlayerDataFor(playerState, pawn, localPos, pos, mc);
        if (pd.distance == 0.0) continue;

        // Pre-project bones to screen coords (avoids W2S in render thread)
        CachedPlayer cp = {};
        cp.pd = pd;
        cp.valid = true;
        cp.screenBaseValid = WorldToScreen(pd.position, cp.screenBase);
        if (pd.hasBones) {
            for (int j = 0; j < 16; j++) {
                cp.screenBones[j].visible = WorldToScreen(pd.bones[j], cp.screenBones[j].s);
            }
        }

        frame.players.push_back(cp);
    }
    frame.hasData = true;
}

void ESPThreadFunc()
{
    while (g_espThreadRunning.load()) {
        int writeIdx = 1 - g_readIdx.load();
        CollectESPData(g_frames[writeIdx]);
        g_readIdx.store(writeIdx);
        g_playerCount = g_frames[writeIdx].playerCount;
        std::this_thread::yield();
    }
}

// ============================================================
// ESP Render (reads from cached frame - no driver calls)
// ============================================================
void RenderESP()
{
    if (!g_settings.enabled) return;

    const ESPFrame& frame = g_frames[g_renderFrameIdx];
    if (!frame.hasData) return;

    g_viewProjectionMatrix = frame.viewProj;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    for (const auto& cp : frame.players) {
        if (!cp.valid) continue;
        const PlayerData& pd = cp.pd;

        ImU32 color = GetPlayerColor(pd, frame.localTeam);

        // Off-screen culling: skip players clearly outside the viewport
        bool onScreen = false;
        if (pd.hasBones) {
            for (int j = 0; j < 16; j += 4) {
                if (cp.screenBones[j].visible) {
                    FVec2 s = cp.screenBones[j].s;
                    if (s.x > -200 && s.x < g_screenWidth + 200 && s.y > -200 && s.y < g_screenHeight + 200) {
                        onScreen = true; break;
                    }
                }
            }
        } else if (cp.screenBaseValid) {
            FVec2 s = cp.screenBase;
            onScreen = (s.x > -200 && s.x < g_screenWidth + 200 && s.y > -200 && s.y < g_screenHeight + 200);
        }
        if (!onScreen) continue;

        if (g_settings.showSkeleton)
            DrawSkeleton(draw, cp.screenBones, pd.hasBones, color);

        float minX = 99999, minY = 99999, maxX = -99999, maxY = -99999;
        int projected = 0;
        if (pd.hasBones) {
            for (int j = 0; j < 16; j++) {
                if (!cp.screenBones[j].visible) continue;
                FVec2 sp = cp.screenBones[j].s;
                if (sp.x < minX) minX = sp.x;
                if (sp.y < minY) minY = sp.y;
                if (sp.x > maxX) maxX = sp.x;
                if (sp.y > maxY) maxY = sp.y;
                projected++;
            }
        }
        if (projected < 4) {
            if (cp.screenBaseValid) {
                FVec2 sh = cp.screenBase;
                float bH = 90.0f;
                FVec2 sf;
                FVec3 footPos = pd.position; footPos.z -= 15.0;
                if (WorldToScreen(footPos, sf)) {
                    bH = sf.y - sh.y;
                    if (bH > 5.0f) {
                        float bW = bH * 0.5f;
                        minX = sh.x - bW / 2; maxX = sh.x + bW / 2;
                        minY = sh.y; maxY = sf.y;
                        projected = 4;
                    }
                }
            }
        }

        if (projected >= 4) {
            float pad = (maxY - minY) * 0.1f;
            float hPad = (maxX - minX) * 0.15f;
            minY -= pad; minX -= hPad; maxX += hPad;

            if (g_settings.showBox) {
                switch (g_settings.boxStyle) {
                case BoxStyle::CORNER:   DrawCornerBox(draw, minX, minY, maxX, maxY, color); break;
                case BoxStyle::FILLED:   DrawFilledBox(draw, minX, minY, maxX, maxY, color); break;
                case BoxStyle::ROUNDED:  DrawRoundedBox(draw, minX, minY, maxX, maxY, color); break;
                case BoxStyle::BOX_3D:   Draw3DBox(draw, pd, color); break;
                }
            }

            if (g_settings.showHealthBar)
                DrawHealthBars(draw, maxX + 4.f, minY, maxY, pd.health, pd.shield);

            float centerX = (minX + maxX) * 0.5f;
            float textY = minY;

            if (g_settings.showStatusIndicators) {
                if (pd.isInVehicle) {
                    textY -= 14.f;
                    DrawText(draw, {centerX, textY}, IM_COL32(100,200,255,255), "[VEHICLE]", true);
                }
                if (pd.isKnocked) {
                    textY -= 14.f;
                    DrawText(draw, {centerX, textY}, IM_COL32(255,80,80,255), "[KNOCKED]", true);
                }
                if (pd.isBot) {
                    textY -= 14.f;
                    DrawText(draw, {centerX, textY}, IM_COL32(255,220,50,255), "[BOT]", true);
                }
            }

            if (g_settings.showPlayerName) {
                textY -= 14.f;
                char nameUtf8[128] = {};
                if (pd.playerName[0] != L'\0')
                    WideCharToMultiByte(CP_UTF8, 0, pd.playerName, -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);
                if (nameUtf8[0])
                    DrawText(draw, {centerX, textY}, IM_COL32(255,255,255,255), nameUtf8, true);
                else if (pd.isBot)
                    DrawText(draw, {centerX, textY}, IM_COL32(255,255,255,255), "Bot", true);
                else
                    DrawText(draw, {centerX, textY}, IM_COL32(255,255,255,255), "Player", true);
            }

            float botY = maxY + 2.f;
            if (g_settings.showDistance) {
                char txt[32];
                sprintf_s(txt, "%.0fm", pd.distance);
                DrawText(draw, {centerX, botY}, IM_COL32(255,255,255,255), txt, true);
                botY += 14.f;
            }

            if (g_settings.showWeaponInfo && pd.weaponName[0] != L'\0') {
                char utf8[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, pd.weaponName, -1, utf8, sizeof(utf8), nullptr, nullptr);
                if (pd.ammoCount > 0) {
                    char buf[300];
                    sprintf_s(buf, "%s [%d]", utf8, pd.ammoCount);
                    DrawText(draw, {centerX, botY}, IM_COL32(200,200,200,255), buf, true);
                } else {
                    DrawText(draw, {centerX, botY}, IM_COL32(200,200,200,255), utf8, true);
                }
                botY += 14.f;
            }

            if (g_settings.showKillCount && pd.killScore > 0) {
                char txt[32];
                sprintf_s(txt, "%d kills", pd.killScore);
                DrawText(draw, {centerX, botY}, IM_COL32(255,200,100,255), txt, true);
            }
        }

        if (g_settings.showSnapline) {
            FVec2 sp;
            if (WorldToScreen(pd.position, sp))
                draw->AddLine({(float)(g_screenWidth/2), (float)g_screenHeight}, {sp.x, sp.y}, color, 1.0f);
        }
    }

    if (frame.hasLocalPlayer && g_settings.showLocalSkeleton) {
        ImU32 localColor = IM_COL32(0, 255, 255, 255);
        DrawSkeleton(draw, frame.localScreenBones, frame.localPlayer.hasBones, localColor);
    }

    // FOV Circle
    if (g_settings.showFovCircle && g_aim.enabled) {
        float cx = g_screenWidth * 0.5f;
        float cy = g_screenHeight * 0.5f;
        float fovRadius = g_aim.fov * (g_screenWidth / 90.0f);
        ImU32 fovCol = IM_COL32(255, 255, 255, 80);
        draw->AddCircle(ImVec2(cx, cy), fovRadius, fovCol, 64, g_settings.fovCircleThickness);
        // Crosshair lines
        float crossLen = 8.0f;
        draw->AddLine(ImVec2(cx - crossLen, cy), ImVec2(cx + crossLen, cy), IM_COL32(255,255,255,60), 1.0f);
        draw->AddLine(ImVec2(cx, cy - crossLen), ImVec2(cx, cy + crossLen), IM_COL32(255,255,255,60), 1.0f);
    }
}

// ============================================================
// Driver Init / Attach
// ============================================================
bool InitializeDriver()
{
    printf("[+] Opening driver device...\n");
    xhdr::g_dev = CreateFileW(L"\\\\.\\xhunter1", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (xhdr::g_dev == INVALID_HANDLE_VALUE) {
        printf("[-] Driver not found! Error: %lu\n", GetLastError());
        return false;
    }
    printf("[+] Driver opened!\n");

    if (!xhdr::SendCmd(xhdr::CMD_REGISTER, nullptr, 0)) {
        printf("[-] CMD_REGISTER failed\n"); return false;
    }
    struct { DWORD pid; DWORD flags; } fp = { GetCurrentProcessId(), 0x8 };
    if (!xhdr::SendCmd(xhdr::CMD_SET_FLAGS, &fp, sizeof(fp))) {
        printf("[-] CMD_SET_FLAGS failed\n"); return false;
    }
    printf("[+] Authenticated!\n");
    return true;
}

bool AttachToFortnite()
{
    printf("[+] Looking for Fortnite via Toolhelp...\n");

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        printf("[-] CreateToolhelp32Snapshot failed\n"); return false;
    }

    uint32_t pid = 0;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (wcsstr(pe.szExeFile, L"FortniteClient") && !wcsstr(pe.szExeFile, L"EAC")) {
                pid = pe.th32ProcessID;
                printf("[+] Found PID=%u (%ls)\n", pid, pe.szExeFile);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (!pid) {
        printf("[-] FortniteClient process not found\n");
        return false;
    }

    // EAC blocks Toolhelp module snapshot. Get image base via PEB.
    HANDLE hProcess = xhdr::GetTargetHandle(pid);
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) {
            printf("[-] Cannot open target process PID=%u\n", pid);
            return false;
        }
    }

    using NtQIP = NTSTATUS(NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);
    static auto NtQueryInformationProcess = (NtQIP)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");

    struct PBI { NTSTATUS ExitStatus; PVOID PebBaseAddress; ULONG_PTR AffinityMask; LONG BasePriority; HANDLE UniqueProcessId; HANDLE InheritedFromUniqueProcessId; };
    PBI pbi = {};
    ULONG retLen = 0;
    NTSTATUS status = NtQueryInformationProcess(hProcess, 0, &pbi, sizeof(pbi), &retLen);

    if (status < 0 || !pbi.PebBaseAddress) {
        printf("[-] NtQueryInformationProcess failed status=0x%08lX\n", (unsigned long)status);
        return false;
    }

    printf("[+] PEB=0x%llX\n", (unsigned long long)pbi.PebBaseAddress);

    uint64_t gameBase = 0;
    if (!xhdr::ProcessRead(pid, (uint64_t)pbi.PebBaseAddress + 0x10, &gameBase, sizeof(gameBase)) || !gameBase) {
        printf("[-] Failed to read ImageBaseAddress from PEB\n");
        return false;
    }

    printf("[+] ImageBase=0x%llX\n", (unsigned long long)gameBase);

    xhdr::ProcessInfo info = {};
    info.pid = pid;
    info.sectionBase = gameBase;
    g_driver.SetFromProcessInfo(info);
    g_targetPID = pid;
    g_gameBase = gameBase;
    printf("[+] Attached PID=%u Base=0x%llX\n", pid, (unsigned long long)gameBase);

    uint64_t enc = Read<uint64_t>(gameBase + offsets::core::UWORLD);
    printf("[DBG] UWorld encrypted=0x%llX\n", (unsigned long long)enc);

    // Test xhunter1 cmd 786 as a potential write command (single, safe test)
    uint64_t uworld = GetUWorld();
    if (uworld) {
        uint64_t gameInstance = Read<uint64_t>(uworld + offsets::core::GameInstance);
        uint64_t localPlayers = Read<uint64_t>(gameInstance + offsets::player::LocalPlayers);
        uint64_t localPlayer  = Read<uint64_t>(localPlayers);
        uint64_t pc           = Read<uint64_t>(localPlayer + offsets::player::PlayerController);
        if (pc) {
            printf("[TEST] Testing cmd 786 for write at PC+0x530...\n");
            float testRot[3] = { 3.0f, 3.0f, 0.0f };

            // Format 2: inline data in params (handle + dst + size + data)
            struct { UINT64 h; UINT64 dst; DWORD sz; float data[3]; } wp2 = {};
            wp2.h   = (UINT64)xhdr::GetTargetHandle(pid);
            wp2.dst = pc + 0x530;
            wp2.sz  = sizeof(testRot);
            memcpy(wp2.data, testRot, sizeof(testRot));
            xhdr::SendCmd(786, &wp2, sizeof(wp2), true);
            DWORD sts2 = xhdr::GetThreadRsp().rsp.status;
            printf("[TEST] Format 2 (inline data): status=0x%08X\n", (unsigned)sts2);
            float v2[3] = {};
            xhdr::ProcessRead(pid, pc + 0x530, v2, sizeof(v2));
            printf("[TEST] Value after format 2: (%.2f, %.2f, %.2f)\n", v2[0], v2[1], v2[2]);

            // Format 3: PID-based (pid + access + dst + src + size)
            struct { UINT32 pid; UINT32 access; UINT64 dst; UINT64 src; DWORD sz; } wp3 = {};
            wp3.pid    = pid;
            wp3.access = 0;
            wp3.dst    = pc + 0x530;
            wp3.src    = (UINT64)testRot;
            wp3.sz     = sizeof(testRot);
            xhdr::SendCmd(786, &wp3, sizeof(wp3), true);
            DWORD sts3 = xhdr::GetThreadRsp().rsp.status;
            printf("[TEST] Format 3 (PID-based): status=0x%08X\n", (unsigned)sts3);
            float v3[3] = {};
            xhdr::ProcessRead(pid, pc + 0x530, v3, sizeof(v3));
            printf("[TEST] Value after format 3: (%.2f, %.2f, %.2f)\n", v3[0], v3[1], v3[2]);

            printf("[TEST] Done\n");
        }
    }
    return true;
}

// ============================================================
// Watermark
// ============================================================
void RenderWatermark()
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float alpha = 0.35f + sinf(g_pulsePhase * 0.5f) * 0.1f;
    ImU32 col = IM_COL32(100, 180, 255, (int)(alpha * 255));
    char buf[128];
    sprintf_s(buf, "FESP | %dms | %d players | %s ViGEm",
        (int)(ImGui::GetIO().Framerate > 0 ? 1000.0f / ImGui::GetIO().Framerate : 0),
        g_playerCount,
        g_vigem.IsReady() ? "G" : "!");
    ImU32 bg = IM_COL32(0, 0, 0, 120);
    ImVec2 sz = ImGui::CalcTextSize(buf);
    float pad = 6.0f;
    dl->AddRectFilled(ImVec2(10 - pad, 10 - pad), ImVec2(10 + sz.x + pad, 10 + sz.y + pad), bg, 4.0f);
    dl->AddRect(ImVec2(10 - pad, 10 - pad), ImVec2(10 + sz.x + pad, 10 + sz.y + pad), col, 4.0f, 0, 1.0f);
    dl->AddText(ImVec2(10, 10), col, buf);
}

// ============================================================
// Animated pulse helper
// ============================================================
ImU32 PulseColor(ImU32 base, float speed, float phaseOff)
{
    float t = sinf(g_pulsePhase * speed + phaseOff) * 0.5f + 0.5f;
    int r = (base >> IM_COL32_R_SHIFT) & 0xFF;
    int g = (base >> IM_COL32_G_SHIFT) & 0xFF;
    int b = (base >> IM_COL32_B_SHIFT) & 0xFF;
    int dim = (int)(255 - t * 80);
    return IM_COL32(r * dim / 255, g * dim / 255, b * dim / 255, 255);
}

// ============================================================
// Menu
// ============================================================
void RenderMenu()
{
    g_pulsePhase += 0.05f;
    if (g_pulsePhase > 1000.0f) g_pulsePhase -= 1000.0f;

    // Smooth menu alpha animation
    float dt = ImGui::GetIO().DeltaTime;
    g_menuAlpha += (g_menuTargetAlpha - g_menuAlpha) * dt * 8.0f;
    if (g_menuAlpha < 0.01f) return;
    float a = g_menuAlpha;

    ImGui::SetNextWindowSize(ImVec2(440, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.93f * a);

    ImGui::Begin("Fortnite ESP v41.10", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // Animated gradient header
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 winSz = ImGui::GetWindowSize();
    float headerH = 48.0f;
    ImU32 c1 = IM_COL32(40, 80, 160, (int)(220 * a));
    ImU32 c2 = IM_COL32(80, 40, 160, (int)(220 * a));
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + winSz.x, p0.y + headerH), c1, c2, c2, c1);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + headerH - 2), ImVec2(p0.x + winSz.x, p0.y + headerH), IM_COL32(0, 0, 0, (int)(40 * a)));

    ImGui::SetCursorPosY(10.0f);
    ImGui::SetCursorPosX(14.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, a));
    ImGui::Text("Fortnite ESP");
    ImGui::SetCursorPosX(14.0f);
    ImGui::PushFont(nullptr);
    ImGui::TextDisabled("v41.10");
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(headerH + 6.0f);

    // Status bar
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    {
        ImU32 dCol = g_driverReady ? PulseColor(IM_COL32(0, 220, 80, 255), 2.0f, 0) : IM_COL32(255, 50, 50, 255);
        int dr = (dCol >> IM_COL32_R_SHIFT) & 0xFF, dg = (dCol >> IM_COL32_G_SHIFT) & 0xFF, db = (dCol >> IM_COL32_B_SHIFT) & 0xFF;
        dl->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + 6, ImGui::GetCursorScreenPos().y + 7), 4, IM_COL32(dr, dg, db, (int)(a * 255)));

        ImGui::Text(" ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, a), "Driver");
        ImGui::SameLine();

        ImU32 vCol = g_vigem.IsReady() ? PulseColor(IM_COL32(0, 200, 255, 255), 2.5f, 1.0f) : IM_COL32(255, 50, 50, 255);
        int vr = (vCol >> IM_COL32_R_SHIFT) & 0xFF, vg = (vCol >> IM_COL32_G_SHIFT) & 0xFF, vb = (vCol >> IM_COL32_B_SHIFT) & 0xFF;
        dl->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + 4, ImGui::GetCursorScreenPos().y + 7), 4, IM_COL32(vr, vg, vb, (int)(a * 255)));
        ImGui::Text(" ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, a), "ViGEm");
        ImGui::SameLine();

        if (g_targetPID) {
            char pidTxt[64]; sprintf_s(pidTxt, "PID: %u", g_targetPID);
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.4f, a), "|");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, a), pidTxt);
            ImGui::SameLine();
        }
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.4f, a), "|");
        ImGui::SameLine();
        char plyTxt[32]; sprintf_s(plyTxt, "%d players", g_playerCount);
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, a), plyTxt);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.4f, a), "|");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, a), "%.0f fps", ImGui::GetIO().Framerate);
    }
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0, 2));
    if (ImGui::Button("Reattach", ImVec2(-1, 0))) AttachToFortnite();
    ImGui::Dummy(ImVec2(0, 2));

    // Proper tab system (content inside BeginTabItem/EndTabItem)
    if (ImGui::BeginTabBar("##tabs")) {

        if (ImGui::BeginTabItem("Visuals")) {
            ImGui::Checkbox("Enable ESP", &g_settings.enabled);
            ImGui::Checkbox("FOV Circle", &g_settings.showFovCircle);
            if (g_settings.showFovCircle) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::SliderFloat("##fovw", &g_settings.fovCircleThickness, 0.5f, 4.0f, "%.1f");
            }
            ImGui::Checkbox("Box", &g_settings.showBox);
            if (g_settings.showBox) {
                ImGui::SameLine();
                int bs = (int)g_settings.boxStyle;
                ImGui::SetNextItemWidth(120);
                if (ImGui::Combo("##boxstyle", &bs, "Corner\0Filled\0Rounded\0 3D\0"))
                    g_settings.boxStyle = (BoxStyle)bs;
            }
            ImGui::Checkbox("Skeleton", &g_settings.showSkeleton);
            ImGui::Checkbox("Distance", &g_settings.showDistance);
            ImGui::Checkbox("Snapline", &g_settings.showSnapline);
            ImGui::Checkbox("Outlined Text", &g_settings.outlinedText);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Colors")) {
            int cm = (int)g_settings.colorMode;
            ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("Color Mode", &cm, "Static\0Distance\0Team\0"))
                g_settings.colorMode = (ColorMode)cm;

            ImGui::Dummy(ImVec2(0, 4));
            if (g_settings.colorMode == ColorMode::STATIC)
                ImGui::ColorEdit4("Box Color", g_settings.boxColor, ImGuiColorEditFlags_NoInputs);
            if (g_settings.colorMode == ColorMode::DISTANCE_BASED) {
                ImGui::SliderFloat("Close (Red)", &g_settings.distClose, 10.f, 100.f);
                ImGui::SliderFloat("Mid (Yellow)", &g_settings.distMid, 50.f, 200.f);
                ImGui::SliderFloat("Far (Green)", &g_settings.distFar, 100.f, 500.f);
            }
            if (g_settings.colorMode == ColorMode::TEAM_BASED)
                ImGui::ColorEdit4("Team Color", g_settings.teamColor, ImGuiColorEditFlags_NoInputs);

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::ColorEdit4("Health Bar", g_settings.healthColor, ImGuiColorEditFlags_NoInputs);
            ImGui::ColorEdit4("Shield Bar", g_settings.shieldColor, ImGuiColorEditFlags_NoInputs);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Info")) {
            ImGui::Checkbox("Player Names", &g_settings.showPlayerName);
            ImGui::Checkbox("Health/Shield Bars", &g_settings.showHealthBar);
            ImGui::Checkbox("Weapon Info", &g_settings.showWeaponInfo);
            ImGui::Checkbox("Status Indicators", &g_settings.showStatusIndicators);
            ImGui::Checkbox("Kill Count", &g_settings.showKillCount);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::SliderInt("Max Distance (m)", &g_settings.maxDistance, 50, 1000);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Skeleton")) {
            ImGui::Checkbox("Show Local Skeleton", &g_settings.showLocalSkeleton);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Checkbox("Gradient Color", &g_settings.skeletonGradient);
            ImGui::Checkbox("Glow Effect", &g_settings.skeletonGlow);
            ImGui::SliderFloat("Line Width", &g_settings.skeletonLineWidth, 0.5f, 4.0f);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Aimbot")) {
            ImGui::Checkbox("Enable Aimbot", &g_aim.enabled);
            ImGui::SameLine();
            if (g_aim.masterEnabled)
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.4f, a), "  [ACTIVE]");
            else
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, a), "  [TOGGLED OFF]");
            ImGui::Dummy(ImVec2(0, 2));

            ImGui::SliderFloat("Smooth Speed", &g_aim.smooth, 0.01f, 0.50f, "%.3f");
            ImGui::SliderFloat("FOV (degrees)", &g_aim.fov, 1.0f, 30.0f, "%.1f");
            ImGui::SliderFloat("Random Skip", &g_aim.randomSkip, 0.0f, 0.80f, "%.2f");
            ImGui::SliderFloat("Jitter", &g_aim.randomOffset, 0.0f, 10.0f, "%.1f");

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, a), "ViGEm Controller");
            if (g_vigem.IsReady()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0, 1, 0.4f, a), "  [OK]");
            }
            ImGui::SliderFloat("Stick Sens.", &g_aim.stickSensitivity, 0.05f, 1.0f, "%.2f");
            ImGui::SliderFloat("Deadzone", &g_aim.stickDeadzone, 0.0f, 0.20f, "%.2f");

            ImGui::Dummy(ImVec2(0, 2));
            ImGui::Checkbox("Aim at teammates", &g_aim.aimAtTeam);
            ImGui::Dummy(ImVec2(0, 2));
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, a * 0.8f), "RMB = hold aim  |  P = toggle on/off");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // Bottom bar with exit button
    ImU32 botCol = IM_COL32(20, 20, 30, (int)(180 * a));
    dl->AddRectFilled(ImVec2(p0.x, ImGui::GetCursorScreenPos().y), ImVec2(p0.x + winSz.x, ImGui::GetCursorScreenPos().y + 30), botCol);
    dl->AddLine(ImVec2(p0.x, ImGui::GetCursorScreenPos().y), ImVec2(p0.x + winSz.x, ImGui::GetCursorScreenPos().y), IM_COL32(255, 255, 255, (int)(15 * a)));

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
    if (ImGui::Button("Exit Cheat", ImVec2(120, 24))) {
        PostQuitMessage(0);
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, a * 0.6f), "closes process completely");

    ImGui::End();
}

// ============================================================
// Main
// ============================================================
int main()
{
    AllocConsole();
    ShowWindow(GetConsoleWindow(), SW_HIDE);
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    g_screenWidth = GetSystemMetrics(SM_CXSCREEN);
    g_screenHeight = GetSystemMetrics(SM_CYSCREEN);

    srand((unsigned int)GetTickCount64());

    g_driverReady = InitializeDriver();
    if (!g_driverReady) {
        Sleep(3000);
        return 1;
    }

    if (g_vigem.Init()) {
        printf("[+] ViGEm ready - aimbot uses virtual Xbox 360 controller\n");
    } else {
        printf("[-] ViGEm init failed - aimbot via controller disabled\n");
    }

    if (!AttachToFortnite())
        printf("[!] Fortnite not running. Use Reattach button later.\n");

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"FESP", nullptr };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED,
        wc.lpszClassName, L"", WS_POPUP,
        0, 0, g_screenWidth, g_screenHeight,
        nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 5);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.16f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.20f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.30f, 0.45f, 1.0f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.22f, 0.22f, 0.35f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.45f, 1.0f);
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    printf("[+] Overlay ready! INSERT=toggle menu\n\n");

    g_espThreadRunning = true;
    g_espThread = std::thread(ESPThreadFunc);

    bool done = false;
    bool showMenu = true;
    static LARGE_INTEGER perfFreq;
    QueryPerformanceFrequency(&perfFreq);
    static LARGE_INTEGER lastFrame;
    QueryPerformanceCounter(&lastFrame);

    while (!done)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (GetAsyncKeyState(VK_INSERT) & 1) {
            showMenu = !showMenu;
            g_menuTargetAlpha = showMenu ? 1.0f : 0.0f;
            LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
            if (showMenu) SetWindowLong(hwnd, GWL_EXSTYLE, style & ~WS_EX_TRANSPARENT);
            else          SetWindowLong(hwnd, GWL_EXSTYLE, style | WS_EX_TRANSPARENT);
        }

        // Frame limiter: sleep if we're ahead of 60fps
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - lastFrame.QuadPart) / perfFreq.QuadPart;
        if (elapsed < (1.0 / 60.0)) {
            DWORD sleepMs = (DWORD)(((1.0 / 60.0) - elapsed) * 1000.0);
            if (sleepMs > 1) Sleep(sleepMs - 1);
        }
        QueryPerformanceCounter(&lastFrame);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        g_renderFrameIdx = g_readIdx.load();
        RenderWatermark();
        if (showMenu) RenderMenu();
        RenderESP();
        RunAimbot();

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        const float cc[4] = { 0, 0, 0, 0 };
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(0, 0);
    }

    g_espThreadRunning = false;
    if (g_espThread.joinable()) g_espThread.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_vigem.Shutdown();
    if (xhdr::g_dev != INVALID_HANDLE_VALUE) CloseHandle(xhdr::g_dev);
    if (xhdr::g_targetH) CloseHandle(xhdr::g_targetH);
    if (g_gameHandle) CloseHandle(g_gameHandle);
    return 0;
}

// ============================================================
// DirectX helpers
// ============================================================
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl,
        &g_pd3dDeviceContext) != S_OK) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* buf;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&buf));
    g_pd3dDevice->CreateRenderTargetView(buf, nullptr, &g_mainRenderTargetView);
    buf->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && g_pd3dDevice) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
