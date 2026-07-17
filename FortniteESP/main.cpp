// main.cpp - Fortnite ESP Box
// Build: v41.20 CL-55550516
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

#include <cstdlib>
#include <algorithm>
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")

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

    BoxStyle boxStyle = BoxStyle::CORNER;
    ColorMode colorMode = ColorMode::DISTANCE_BASED;

    float boxColor[4] = { 1.0f, 0.2f, 0.2f, 1.0f };
    float healthColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    float shieldColor[4] = { 0.0f, 0.4f, 1.0f, 1.0f };
    float teamColor[4] = { 0.0f, 1.0f, 1.0f, 1.0f };

    float skeletonLineWidth = 1.5f;
    bool skeletonGradient = true;
    bool skeletonGlow = true;
    bool outlinedText = true;

    float distClose = 50.0f;
    float distMid = 150.0f;
    float distFar = 300.0f;

    int maxDistance = 300;
} g_settings;

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

    wchar_t playerName[64];
    wchar_t weaponName[128];
    int ammoCount;
    bool isReloading;

    float health;
    float shield;

    FVec3 bones[16];
    bool hasBones;
};

struct AimbotSettings {
    bool enabled = true;
    float smooth = 0.10f;      // 0.01=very slow 0.5=fast
    float fov = 15.0f;         // degrees from crosshair
    int aimKey = VK_RBUTTON;   // RMB by default
    bool autoFire = false;
    bool aimAtTeam = false;    // allow aiming at teammates
    bool aimThroughWalls = true;
    bool visibleCheck = false;
    float randomSkip = 0.00f;  // skip frames randomly
    float randomOffset = 0.5f; // max degrees of random noise
    float stickSensitivity = 0.85f; // ViGEm right stick sensitivity (0.1-1.0)
    float stickDeadzone = 0.06f;    // deadzone fraction (0.0-0.2)
    int aimToggleKey = 0x50;        // 'P' key to toggle aimbot master switch
    bool masterEnabled = true;      // toggled by aimToggleKey
} g_aim;

ViGEmManager g_vigem;
Driver g_driver;
bool g_driverReady = false;
uint32_t g_targetPID = 0;
uint64_t g_gameBase = 0;
int g_playerCount = 0;
int g_renderFrameIdx = 0;

struct ScreenBone { FVec2 s; bool visible; };

struct CachedPlayer {
    PlayerData pd;
    uint64_t pawn = 0;
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
    bool hasLocalPlayer;
};

static ESPFrame g_frames[2];
static std::atomic<int> g_readIdx{0};
static std::mutex g_writeMtx;
static std::atomic<bool> g_espThreadRunning{false};
static std::thread g_espThread;

// ============================================================
// Memory read helper
// ============================================================
template<typename T>
T Read(uint64_t addr) {
    T val{};
    if (!g_targetPID || !addr) return val;
    xhdr::ProcessRead(g_targetPID, addr, &val, sizeof(T));
    return val;
}

inline bool ReadBuffer(uint64_t addr, void* dst, uint32_t size) {
    if (!g_targetPID || !addr || !dst || !size) return false;
    return xhdr::ProcessRead(g_targetPID, addr, dst, size);
}

// ============================================================
// GEngine helpers
// ============================================================
uint64_t GetViewport()
{
    if (!g_targetPID || !g_gameBase) return 0;
    uint64_t engine = Read<uint64_t>(g_gameBase + offsets::core::gEngine);
    if (!engine) return 0;
    return Read<uint64_t>(engine + offsets::core::GameViewport);
}

uint64_t GetUWorld()
{
    uint64_t viewport = GetViewport();
    if (!viewport) return 0;
    return Read<uint64_t>(viewport + offsets::core::ViewportClient);
}

uint64_t GetGameInstance()
{
    uint64_t viewport = GetViewport();
    if (!viewport) return 0;
    return Read<uint64_t>(viewport + 0x80);  // UGameViewportClient::GameInstance
}

void DebugUWorld()
{
    if (!g_targetPID || !g_gameBase) {
        printf("[-] No target\n"); return;
    }
    printf("[DBG] gameBase=0x%llX\n", (unsigned long long)g_gameBase);
    uint64_t engine = Read<uint64_t>(g_gameBase + offsets::core::gEngine);
    printf("[DBG] GEngine=0x%llX\n", (unsigned long long)engine);
    if (!engine) { printf("[-] GEngine is NULL\n"); return; }
    uint64_t viewport = Read<uint64_t>(engine + offsets::core::GameViewport);
    printf("[DBG] GameViewport(0xB70)=0x%llX\n", (unsigned long long)viewport);
    uint64_t gameInst = Read<uint64_t>(engine + 0x288);
    printf("[DBG] GameInstance(0x288)=0x%llX\n", (unsigned long long)gameInst);
    // Try reading UWorld from viewport
    if (viewport) {
        uint64_t uworld = Read<uint64_t>(viewport + offsets::core::ViewportClient);
        printf("[DBG] UWorld(viewport)=0x%llX\n", (unsigned long long)uworld);
        if (uworld) {
            uint64_t level = Read<uint64_t>(uworld + offsets::core::PersistentLevel);
            uint64_t gs = Read<uint64_t>(uworld + offsets::core::GameState);
            printf("[DBG] PersistentLevel=0x%llX GameState(0x1D0)=0x%llX\n",
                (unsigned long long)level, (unsigned long long)gs);
            // Scan for GameState
            printf("[DBG] Scanning for GameState near UWorld+0x1B0..0x210:\n");
            for (uintptr_t off = 0x1B0; off <= 0x210; off += 8) {
                uint64_t val = Read<uint64_t>(uworld + off);
                if (val && val >= 0x7FF000000000 && val <= 0x800000000000) {
                    uint64_t pa = Read<uint64_t>(val + 0x288);
                    printf("[DBG]   +0x%02llX = 0x%llX  PlayerArray=0x%llX\n",
                        (unsigned long long)off, (unsigned long long)val, (unsigned long long)pa);
                }
            }
            printf("[DBG] GameInstance from UWorld+0x248=0x%llX\n",
                (unsigned long long)Read<uint64_t>(uworld + 0x248));
            printf("[DBG] GameInstance from viewport+0x80=0x%llX\n",
                (unsigned long long)Read<uint64_t>(viewport + 0x80));
            printf("[DBG] Scanning for Camera TArray near UWorld+0x180..0x1A0:\n");
            for (uintptr_t off = 0x180; off <= 0x1A0; off += 8) {
                uint64_t data = Read<uint64_t>(uworld + off);
                int32_t cnt = Read<int32_t>(uworld + off + 0x8);
                if (data && cnt > 0 && cnt < 100) {
                    FMatrix m = Read<FMatrix>(data + 256);
                    printf("[DBG]   +0x%02llX: data=0x%llX cnt=%d m[0][0]=%.1f\n",
                        (unsigned long long)off, (unsigned long long)data, cnt, m.m[0][0]);
                }
            }
        }
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
    c.rot = Read<FQuat>(mesh + offsets::core::ComponentToWorld);
    c.pos = Read<FVec3>(mesh + offsets::core::ComponentToWorld + 0x20);
    c.boneArray = Read<uint64_t>(mesh + offsets::core::BoneArray_cache);
    if (!c.boneArray)
        c.boneArray = Read<uint64_t>(mesh + offsets::core::BoneArray);
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
// Aimbot
// ============================================================
#define M_PI 3.14159265358979323846

FRotator CalculateAimAngles(FVec3 cameraPos, FVec3 targetPos, FRotator currentRot)
{
    FVec3 delta = { targetPos.x - cameraPos.x, targetPos.y - cameraPos.y, targetPos.z - cameraPos.z };
    double dist = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (dist < 1.0) return currentRot;
    double pitch = -asin(delta.z / dist) * (180.0 / M_PI);
    double yaw = atan2(delta.y, delta.x) * (180.0 / M_PI);
    if (pitch > 89.0) pitch = 89.0;
    if (pitch < -89.0) pitch = -89.0;
    if (yaw < 0.0) yaw += 360.0;
    return { pitch, yaw, 0.0 };
}

FRotator SmoothRot(FRotator current, FRotator target, float smoothFactor)
{
    double dp = target.pitch - current.pitch;
    double dy = target.yaw - current.yaw;
    while (dy > 180.0) dy -= 360.0;
    while (dy < -180.0) dy += 360.0;
    current.pitch += dp * smoothFactor;
    current.yaw += dy * smoothFactor;
    if (current.pitch > 89.0) current.pitch = 89.0;
    if (current.pitch < -89.0) current.pitch = -89.0;
    if (current.yaw < 0.0) current.yaw += 360.0;
    if (current.yaw >= 360.0) current.yaw -= 360.0;
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
    static float prevNX = 0.0f, prevNY = 0.0f;
    bool keyDown = (GetAsyncKeyState(g_aim.aimKey) & 0x8000) != 0;
    if (!keyDown) {
        if (keyHeld) {
            XUSB_REPORT report = {};
            g_vigem.Update(report);
            keyHeld = false;
        }
        prevNX = 0.0f; prevNY = 0.0f;
        return;
    }
    if (!keyHeld) {
        keyHeld = true;
        prevNX = 0.0f; prevNY = 0.0f;
    }

    const ESPFrame& frame = g_frames[g_renderFrameIdx];
    if (!frame.hasData) return;

    if ((rand() % 1000) < (int)(g_aim.randomSkip * 1000.0f)) return;

    // FOV radius in pixels (matches the visual FOV circle)
    float fovRadius = g_aim.fov * (g_screenWidth / 90.0f);
    float bestDist = fovRadius;
    uint64_t bestPawn = 0;
    FVec2 bestScreen = {};

    // Target stickiness: prefer current target unless another is 40% closer
    static uint64_t lockedPawn = 0;

    for (size_t i = 0; i < frame.players.size(); i++) {
        const CachedPlayer& cp = frame.players[i];
        if (!cp.valid) continue;
        if (!g_aim.aimAtTeam && cp.pd.teamIndex == frame.localTeam && frame.localTeam != 0) continue;

        FVec3 targetPos;
        bool hasPos = false;
        if (cp.pd.hasBones) {
            targetPos = cp.pd.bones[offsets::aimbot::BONE_HEAD];
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
            targetPos.z += 170.0; // approximate head height when no bones
            hasPos = (targetPos.x != 0.0 || targetPos.y != 0.0 || targetPos.z != 0.0);
        }
        if (!hasPos) continue;

        FVec2 screen;
        if (!WorldToScreen(targetPos, screen)) continue;

        float screenDist = ScreenDistToCrosshair(screen);
        // Bias toward locked target (40% advantage = new target must be 40% closer to steal)
        if (cp.pawn == lockedPawn) screenDist *= 0.6f;
        if (screenDist < bestDist) {
            bestDist = screenDist;
            bestPawn = cp.pawn;
            bestScreen = screen;
        }
    }

    if (!bestPawn) {
        lockedPawn = 0;
        XUSB_REPORT report = {};
        g_vigem.Update(report);
        prevNX = prevNX * 0.5f; prevNY = prevNY * 0.5f;
        return;
    }
    if (lockedPawn != bestPawn) {
        prevNX = 0.0f; prevNY = 0.0f;
    }
    lockedPawn = bestPawn;

    // Find the locked target in the current frame (robust against sorting)
    const CachedPlayer* bestCp = nullptr;
    for (const auto& cp : frame.players) {
        if (cp.pawn == bestPawn && cp.valid) { bestCp = &cp; break; }
    }
    if (!bestCp) return;
    FVec3 targetPos;
    bool hasPos = false;
    if (bestCp->pd.hasBones) {
        targetPos = bestCp->pd.bones[offsets::aimbot::BONE_HEAD];
        hasPos = (targetPos.x != 0.0 || targetPos.y != 0.0 || targetPos.z != 0.0);
    }
    if (!hasPos && bestCp->pd.hasBones) {
        targetPos = bestCp->pd.bones[offsets::aimbot::BONE_CHEST];
        hasPos = (targetPos.x != 0.0 || targetPos.y != 0.0 || targetPos.z != 0.0);
    }
    if (!hasPos) {
        targetPos = bestCp->pd.position;
        targetPos.z += 170.0; // approximate head height when no bones
        hasPos = (targetPos.x != 0.0 || targetPos.y != 0.0 || targetPos.z != 0.0);
    }
    if (!hasPos) return;

    FVec2 screen;
    if (!WorldToScreen(targetPos, screen)) return;
    bestScreen = screen;

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
        if (g_aim.autoFire) report.wButtons = XUSB_GAMEPAD_A;
        g_vigem.Update(report);
        prevNX = prevNX * 0.5f; prevNY = prevNY * 0.5f;
        return;
    }

    // Linear P-controller for aimbot: deflection is proportional to on-screen distance.
    // This naturally slows down as it approaches the target, preventing overshoot.
    float floorDeflect = 0.18f;                       // always move a bit so small errors get corrected
    float farDist = 120.0f;                           // distance at which we hit max stick deflection
    float t = (pixelDist - deadzonePx) / (farDist - deadzonePx);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float targetDeflect = floorDeflect + (g_aim.stickSensitivity - floorDeflect) * t;

    // Small hard cap very close to target to kill any residual oscillation
    if (pixelDist < 8.0f && targetDeflect > 0.35f)
        targetDeflect = 0.35f;

    float targetNX = (dx / pixelDist) * targetDeflect;
    float targetNY = (dy / pixelDist) * targetDeflect;

    // Adaptive output smoothing: less inertia when far (prevents overshoot),
    // more inertia when close (keeps it smooth).
    float alphaClose = 0.55f + g_aim.smooth * 0.20f;
    float alphaFar   = 0.78f + g_aim.smooth * 0.10f;
    float alphaT = (pixelDist - 30.0f) / (80.0f - 30.0f);
    if (alphaT < 0.0f) alphaT = 0.0f;
    if (alphaT > 1.0f) alphaT = 1.0f;
    float alpha = alphaClose + (alphaFar - alphaClose) * alphaT;

    float nx = prevNX + (targetNX - prevNX) * alpha;
    float ny = prevNY + (targetNY - prevNY) * alpha;
    prevNX = nx;
    prevNY = ny;

    if (nx > 1.0f) nx = 1.0f; if (nx < -1.0f) nx = -1.0f;
    if (ny > 1.0f) ny = 1.0f; if (ny < -1.0f) ny = -1.0f;

    XUSB_REPORT report = {};
    report.sThumbRX = (SHORT)(nx * 32767);
    report.sThumbRY = (SHORT)(-ny * 32767);
    g_vigem.Update(report);
}

// ============================================================
// Outlined text helper
// ============================================================
void DrawOutlinedText(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, bool centered = false)
{
    if (centered) {
        ImVec2 sz = ImGui::CalcTextSize(text);
        pos.x -= sz.x * 0.5f;
    }
    if (g_settings.outlinedText) {
        ImU32 bg = IM_COL32(0, 0, 0, 200);
        dl->AddText(ImVec2(pos.x - 1, pos.y), bg, text);
        dl->AddText(ImVec2(pos.x + 1, pos.y), bg, text);
        dl->AddText(ImVec2(pos.x, pos.y - 1), bg, text);
        dl->AddText(ImVec2(pos.x, pos.y + 1), bg, text);
    }
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
void DrawSkeleton(ImDrawList* dl, const PlayerData& pd, ImU32 baseCol)
{
    if (!pd.hasBones) return;
    struct Conn { int a, b; };
    Conn conns[] = {
        {0,1},{1,2},{2,3},{1,4},{4,5},{5,6},{1,7},{7,8},{8,9},
        {3,10},{10,11},{11,12},{3,13},{13,14},{14,15}
    };
    for (auto& cn : conns) {
        const FVec3& a = pd.bones[cn.a];
        const FVec3& b = pd.bones[cn.b];
        if (a.x == 0.0 && a.y == 0.0 && a.z == 0.0) continue;
        if (b.x == 0.0 && b.y == 0.0 && b.z == 0.0) continue;
        FVec2 sa, sb;
        if (!WorldToScreen(const_cast<FVec3&>(a), sa) || !WorldToScreen(const_cast<FVec3&>(b), sb)) continue;

        ImU32 lineCol = baseCol;
        if (g_settings.skeletonGradient) {
            float t = (float)cn.a / 15.0f;
            int r = (int)(255 * (1.0f - t * 0.5f));
            int g = (int)(180 * (1.0f - fabs(t - 0.5f) * 2.0f) + 75);
            int bv = (int)(255 * t);
            lineCol = IM_COL32(r, g, bv, 255);
        }
        if (g_settings.skeletonGlow)
            dl->AddLine({sa.x,sa.y}, {sb.x,sb.y}, IM_COL32(0,0,0,120), g_settings.skeletonLineWidth + 2.5f);
        dl->AddLine({sa.x,sa.y}, {sb.x,sb.y}, lineCol, g_settings.skeletonLineWidth);
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
// Position helpers with vehicle fallback
// ============================================================
FVec3 GetActorPosition(uint64_t actor)
{
    FVec3 result = {};
    if (!actor) return result;
    uint64_t rootComp = Read<uint64_t>(actor + offsets::core::RootComponent);
    if (!rootComp) return result;
    return Read<FVec3>(rootComp + offsets::core::RelativeLocation);
}

FVec3 GetPawnPosition(uint64_t pawn)
{
    FVec3 pos = GetActorPosition(pawn);
    if (pos.x != 0.0 || pos.y != 0.0 || pos.z != 0.0) return pos;
    // Player may be in a vehicle; try the vehicle position
    uint64_t vehicle = Read<uint64_t>(pawn + offsets::player::CurrentVehicle);
    if (vehicle) return GetActorPosition(vehicle);
    return pos;
}

// ============================================================
// Read all player data
// ============================================================
PlayerData ReadPlayerDataFor(uint64_t playerState, uint64_t pawn, FVec3 localPos)
{
    PlayerData pd = {};
    pd.health = 100.f;
    pd.shield = 0.f;
    pd.playerName[0] = L'\0';

    pd.position = GetPawnPosition(pawn);
    if (pd.position.x == 0.0 && pd.position.y == 0.0 && pd.position.z == 0.0) return pd;

    double dx = pd.position.x - localPos.x;
    double dy = pd.position.y - localPos.y;
    double dz = pd.position.z - localPos.z;
    pd.distance = sqrt(dx * dx + dy * dy + dz * dz) / 100.0;

    pd.teamIndex = Read<uint8_t>(playerState + offsets::player::TeamIndex);
    pd.killScore = Read<int32_t>(playerState + offsets::player::KillScore);
    pd.isBot = (Read<uint8_t>(playerState + offsets::player::bIsABot) & 0x8) != 0;
    pd.isKnocked = (Read<uint8_t>(pawn + offsets::player::bIsDBNO) & 0x80) != 0;
    pd.isCrouched = (Read<uint8_t>(pawn + offsets::player::bIsCrouched) & 0x2) != 0;
    pd.isSliding = (Read<uint8_t>(pawn + offsets::player::bIsSliding) & 0x10) != 0;
    pd.isSkydiving = (Read<uint8_t>(pawn + offsets::player::bIsSkydiving) & 0x1) != 0;

    uint64_t weapon = Read<uint64_t>(pawn + offsets::player::CurrentWeapon);
    if (weapon) {
        uint64_t wData = Read<uint64_t>(weapon + offsets::weapon::WeaponData);
        if (wData) ReadFText(wData + offsets::weapon::ItemName, pd.weaponName, 128);
        pd.ammoCount = Read<int32_t>(weapon + offsets::weapon::AmmoCount);
        pd.isReloading = (Read<uint8_t>(weapon + offsets::weapon::bIsReloading) & 0x1) != 0;
    }

    pd.hasBones = false;
    if (pd.distance < 350.0f) {
        uint64_t mesh = Read<uint64_t>(pawn + offsets::player::Mesh);
        if (mesh) {
            MeshCache mc = GetMeshCache(mesh);
            if (mc.valid) {
                // Full skeleton only for close players; far players only need head for aimbot
                if (pd.distance < 100.0f) {
                    int ids[] = { BONE_HEAD, BONE_NECK, BONE_CHEST, BONE_PELVIS,
                        BONE_L_SHOULDER, BONE_L_ELBOW, BONE_L_HAND,
                        BONE_R_SHOULDER, BONE_R_ELBOW, BONE_R_HAND,
                        BONE_L_THIGH, BONE_L_KNEE, BONE_L_FOOT,
                        BONE_R_THIGH, BONE_R_KNEE, BONE_R_FOOT };
                    for (int j = 0; j < 16; j++)
                        pd.bones[j] = GetBonePosFromCache(mc, ids[j]);
                    pd.hasBones = (pd.bones[0].x != 0.0 || pd.bones[0].y != 0.0 || pd.bones[0].z != 0.0);
                } else {
                    // Mid/far: just read head to save driver calls
                    pd.bones[0] = GetBonePosFromCache(mc, BONE_HEAD);
                    pd.hasBones = (pd.bones[0].x != 0.0 || pd.bones[0].y != 0.0 || pd.bones[0].z != 0.0);
                }
            }
        }
    }
    return pd;
}

// ============================================================
// Background ESP data collection
// ============================================================
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

    uint64_t gameInstance = GetGameInstance();
    if (!gameInstance) return;
    uint64_t localPlayersArr = Read<uint64_t>(gameInstance + offsets::player::LocalPlayers);
    if (!localPlayersArr) return;
    uint64_t localPlayer = Read<uint64_t>(localPlayersArr);
    if (!localPlayer) return;
    uint64_t playerController = Read<uint64_t>(localPlayer + offsets::player::PlayerController);
    if (!playerController) return;
    uint64_t localPawn = Read<uint64_t>(playerController + offsets::player::LocalPawn);

    frame.viewProj = {};

    FVec3 localPos = {};
    frame.localTeam = 0;
    if (localPawn) {
        localPos = GetPawnPosition(localPawn);
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
                for (int j = 0; j < 16; j++)
                    frame.localPlayer.bones[j] = GetBonePosFromCache(localMC, ids[j]);
                frame.localPlayer.hasBones = true;
                frame.localPlayer.position = localPos;
                frame.hasLocalPlayer = true;
            }
        }
    }

    // Collect all players within maxDistance, then keep the closest ones.
    // This ensures nearby enemies always get ESP even in 100-player lobbies.
    const int MAX_RENDER_PLAYERS = 40;
    frame.players.reserve(playerCount < MAX_RENDER_PLAYERS ? playerCount : MAX_RENDER_PLAYERS);

    for (int i = 0; i < playerCount; i++) {
        uint64_t playerState = Read<uint64_t>(playerArrayData + i * 8);
        if (!playerState) continue;

        uint64_t pawn = Read<uint64_t>(playerState + offsets::player::PawnPrivate);
        if (!pawn || pawn == localPawn) continue;

        // Quick distance check before expensive read
        FVec3 pos = GetPawnPosition(pawn);
        if (pos.x == 0.0 && pos.y == 0.0 && pos.z == 0.0) continue;
        double dx2 = pos.x - localPos.x, dy2 = pos.y - localPos.y, dz2 = pos.z - localPos.z;
        double dist = sqrt(dx2 * dx2 + dy2 * dy2 + dz2 * dz2) / 100.0;
        if (dist > g_settings.maxDistance || dist < 1.0) continue;

        PlayerData pd = ReadPlayerDataFor(playerState, pawn, localPos);
        if (pd.position.x == 0.0 && pd.position.y == 0.0 && pd.position.z == 0.0) continue;

        CachedPlayer cp = {};
        cp.pd = pd;
        cp.pawn = pawn;
        cp.valid = true;
        frame.players.push_back(cp);
    }

    // Sort by distance so the closest threats always have ESP when player count exceeds cap
    std::sort(frame.players.begin(), frame.players.end(),
        [](const CachedPlayer& a, const CachedPlayer& b) {
            return a.pd.distance < b.pd.distance;
        });
    if ((int)frame.players.size() > MAX_RENDER_PLAYERS)
        frame.players.resize(MAX_RENDER_PLAYERS);

    frame.hasData = true;
}

void ESPThreadFunc()
{
    while (g_espThreadRunning.load()) {
        int writeIdx = 1 - g_readIdx.load();
        CollectESPData(g_frames[writeIdx]);
        if (g_frames[writeIdx].hasData) {
            g_playerCount = g_frames[writeIdx].playerCount;
        }
        g_readIdx.store(writeIdx);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

// ============================================================
// Read current camera view matrix from the game (render-thread)
// ============================================================
FMatrix GetCurrentViewProj()
{
    if (!g_targetPID) return g_frames[g_renderFrameIdx].viewProj;
    uint64_t uworld = GetUWorld();
    if (!uworld) return g_frames[g_renderFrameIdx].viewProj;
    uint64_t viewArrayData = Read<uint64_t>(uworld + offsets::core::CachedViewInfoRenderedLastFrame);
    int32_t viewArrayCount = Read<int32_t>(uworld + offsets::core::CachedViewInfoRenderedLastFrame + 0x8);
    if (!viewArrayData || viewArrayCount <= 0) return g_frames[g_renderFrameIdx].viewProj;
    FMatrix mat = Read<FMatrix>(viewArrayData + 256);
    if (mat.m[3][3] == 0.0) return g_frames[g_renderFrameIdx].viewProj;
    return mat;
}

// ============================================================
// ESP Render (reads from cached frame - no driver calls)
// ============================================================
void RenderESP()
{
    if (!g_settings.enabled) return;

    const ESPFrame& frame = g_frames[g_renderFrameIdx];
    if (!frame.hasData) return;

    // Read current view matrix every frame so ESP stays glued to players
    g_viewProjectionMatrix = GetCurrentViewProj();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    for (const auto& cp : frame.players) {
        if (!cp.valid) continue;
        const PlayerData& pd = cp.pd;

        ImU32 color = GetPlayerColor(pd, frame.localTeam);

        if (g_settings.showSkeleton && pd.distance < 100.0f)
            DrawSkeleton(draw, pd, color);

        float minX = 99999, minY = 99999, maxX = -99999, maxY = -99999;
        int projected = 0;
        if (pd.hasBones) {
            int cornerBones[] = { 0, 3, 4, 7 };
            for (int j = 0; j < 4; j++) {
                int idx = cornerBones[j];
                if (pd.bones[idx].x == 0.0 && pd.bones[idx].y == 0.0 && pd.bones[idx].z == 0.0) continue;
                FVec2 sp;
                if (WorldToScreen(pd.bones[idx], sp)) {
                    if (sp.x < minX) minX = sp.x;
                    if (sp.y < minY) minY = sp.y;
                    if (sp.x > maxX) maxX = sp.x;
                    if (sp.y > maxY) maxY = sp.y;
                    projected++;
                }
            }
        }
        if (projected < 4) {
            FVec3 headPos = pd.position; headPos.z += 180.0;
            FVec3 footPos = pd.position; footPos.z -= 15.0;
            FVec2 sh, sf;
            if (WorldToScreen(headPos, sh) && WorldToScreen(footPos, sf)) {
                float bH = sf.y - sh.y;
                if (bH > 5.0f) {
                    float bW = bH * 0.5f;
                    minX = sh.x - bW / 2; maxX = sh.x + bW / 2;
                    minY = sh.y; maxY = sf.y;
                    projected = 4;
                }
            }
        }

        if (projected >= 4) {
            float pad = (maxY - minY) * 0.1f;
            float hPad = (maxX - minX) * 0.15f;
            minY -= pad; minX -= hPad; maxX += hPad;

            // Cull players whose box is completely off-screen (with small margin)
            const float margin = 100.0f;
            bool onScreen = !(maxX < -margin || minX > g_screenWidth + margin ||
                              maxY < -margin || minY > g_screenHeight + margin);

            if (onScreen && g_settings.showBox) {
                switch (g_settings.boxStyle) {
                case BoxStyle::CORNER:   DrawCornerBox(draw, minX, minY, maxX, maxY, color); break;
                case BoxStyle::FILLED:   DrawFilledBox(draw, minX, minY, maxX, maxY, color); break;
                case BoxStyle::ROUNDED:  DrawRoundedBox(draw, minX, minY, maxX, maxY, color); break;
                case BoxStyle::BOX_3D:   Draw3DBox(draw, pd, color); break;
                }
            }

            if (onScreen) {
                if (g_settings.showHealthBar)
                    DrawHealthBars(draw, maxX + 4.f, minY, maxY, pd.health, pd.shield);

                float centerX = (minX + maxX) * 0.5f;
                float textY = minY;

                if (g_settings.showStatusIndicators) {
                    if (pd.isKnocked) {
                        textY -= 14.f;
                        DrawOutlinedText(draw, {centerX, textY}, IM_COL32(255,80,80,255), "[KNOCKED]", true);
                    }
                    if (pd.isBot) {
                        textY -= 14.f;
                        DrawOutlinedText(draw, {centerX, textY}, IM_COL32(255,220,50,255), "[BOT]", true);
                    }
                }

                if (g_settings.showPlayerName) {
                    textY -= 14.f;
                    char nameUtf8[128] = {};
                    if (pd.playerName[0] != L'\0')
                        WideCharToMultiByte(CP_UTF8, 0, pd.playerName, -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);
                    if (nameUtf8[0])
                        DrawOutlinedText(draw, {centerX, textY}, IM_COL32(255,255,255,255), nameUtf8, true);
                    else if (pd.isBot)
                        DrawOutlinedText(draw, {centerX, textY}, IM_COL32(255,255,255,255), "Bot", true);
                    else
                        DrawOutlinedText(draw, {centerX, textY}, IM_COL32(255,255,255,255), "Player", true);
                }

                float botY = maxY + 2.f;
                if (g_settings.showDistance) {
                    char txt[32];
                    sprintf_s(txt, "%.0fm", pd.distance);
                    DrawOutlinedText(draw, {centerX, botY}, IM_COL32(255,255,255,255), txt, true);
                    botY += 14.f;
                }

                if (g_settings.showWeaponInfo && pd.weaponName[0] != L'\0') {
                    char utf8[256] = {};
                    WideCharToMultiByte(CP_UTF8, 0, pd.weaponName, -1, utf8, sizeof(utf8), nullptr, nullptr);
                    if (pd.ammoCount > 0) {
                        char buf[300];
                        sprintf_s(buf, "%s [%d]", utf8, pd.ammoCount);
                        DrawOutlinedText(draw, {centerX, botY}, IM_COL32(200,200,200,255), buf, true);
                    } else {
                        DrawOutlinedText(draw, {centerX, botY}, IM_COL32(200,200,200,255), utf8, true);
                    }
                    botY += 14.f;
                }

                if (g_settings.showKillCount && pd.killScore > 0) {
                    char txt[32];
                    sprintf_s(txt, "%d kills", pd.killScore);
                    DrawOutlinedText(draw, {centerX, botY}, IM_COL32(255,200,100,255), txt, true);
                }
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
        DrawSkeleton(draw, frame.localPlayer, localColor);
    }
}

// ============================================================
// Driver Init / Attach
// ============================================================
bool InitializeDriver()
{
    printf("[+] Opening driver device...\n");
    xhdr::g_dev = CreateFileW(L"\\\\.\\xHunters", GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

    if (!NtoskrnlParser::Resolve(xhdr::g_off)) {
        printf("[-] EPROCESS offsets failed\n"); return false;
    }
    xhdr::g_off.Dump();

    xhdr::g_ntBase = xhdr::GetNtoskrnlBase();
    if (!xhdr::g_ntBase) { printf("[-] ntoskrnl base failed\n"); return false; }
    printf("[+] ntoskrnl: 0x%llX\n", xhdr::g_ntBase);

    uint64_t ptr = xhdr::g_ntBase + xhdr::g_off.PsInitialSystemProcessRVA;
    if (!xhdr::KernelRead(ptr, &xhdr::g_sysEproc, 8)) {
        printf("[-] System EPROCESS failed\n"); return false;
    }
    printf("[+] System EPROCESS: 0x%llX\n", xhdr::g_sysEproc);
    return true;
}

bool AttachToFortnite()
{
    printf("[+] Looking for Fortnite...\n");
    xhdr::ProcessInfo info;
    // EPROCESS.ImageFileName is 15 bytes (truncated).
    // Walk process list manually looking for partial match.
    // The kernel stores "FortniteClient" (truncated from full exe name)
    
    // Walk EPROCESS list ourselves with strstr for partial match
    if (!xhdr::g_sysEproc || !xhdr::g_off.valid) {
        printf("[-] Driver not initialized\n"); return false;
    }
    
    uint64_t curVA = xhdr::g_sysEproc;
    bool found = false;
    for (int i = 0; i < 1024; i++) {
        char imgName[16] = {};
        DWORD pid = 0;
        xhdr::KernelRead(curVA + xhdr::g_off.ImageFileName, imgName, sizeof(imgName));
        xhdr::KernelRead(curVA + xhdr::g_off.UniqueProcessId, &pid, sizeof(pid));
        
        // Match "FortniteClient" specifically (not FortniteBootst or FortniteLauncher)
        if (strstr(imgName, "Client") || strstr(imgName, "client")) {
            // Skip zombies
            LONG exitStatus = 0;
            xhdr::KernelRead(curVA + xhdr::g_off.ExitStatus, &exitStatus, sizeof(exitStatus));
            if (exitStatus != 0x103) {
                printf("[!] Skipping PID=%u (exit=0x%X)\n", pid, (unsigned)exitStatus);
            } else {
                // Don't break - keep looking. The REAL game process is the last
                // "FortniteClient" in the list (EAC bootstrapper spawns first).
                info.pid = pid;
                info.eprocess = curVA;
                xhdr::KernelRead(curVA + xhdr::g_off.DirectoryTableBase, &info.cr3, sizeof(info.cr3));
                xhdr::KernelRead(curVA + xhdr::g_off.Peb, &info.peb, sizeof(info.peb));
                xhdr::KernelRead(curVA + xhdr::g_off.SectionBaseAddress, &info.sectionBase, sizeof(info.sectionBase));
                printf("[+] Candidate: '%s' PID=%u Base=0x%llX\n", imgName, pid, (unsigned long long)info.sectionBase);
                found = true;
                // Keep going - we want the LAST match (actual game, not EAC)
            }
        }
        
        uint64_t flink = 0;
        if (!xhdr::KernelRead(curVA + xhdr::g_off.ActiveProcessLinks, &flink, sizeof(flink)) || !flink) break;
        if (flink == xhdr::g_sysEproc + xhdr::g_off.ActiveProcessLinks) break;
        uint64_t nextVA = flink - xhdr::g_off.ActiveProcessLinks;
        if (nextVA == curVA) break;
        curVA = nextVA;
    }
    
    if (!found) {
        printf("[-] Fortnite not found in EPROCESS list!\n");
        return false;
    }
    printf("[+] Selected: PID=%u Base=0x%llX\n", info.pid, (unsigned long long)info.sectionBase);

    g_driver.SetFromProcessInfo(info);
    g_targetPID = info.pid;
    g_gameBase = info.sectionBase;

    // Test UWorld decryption
    DebugUWorld();
    uint64_t uworld = GetUWorld();
    if (uworld)
        printf("[+] UWorld: 0x%llX (decrypted OK)\n", uworld);
    else
        printf("[-] UWorld decrypt returned NULL\n");
    return true;
}

// ============================================================
// Menu
// ============================================================
void RenderMenu()
{
    ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
    ImGui::Begin("Fortnite ESP v41.20", nullptr, ImGuiWindowFlags_NoCollapse);

    if (g_driverReady) ImGui::TextColored(ImVec4(0,1,0,1), "Driver: OK");
    else               ImGui::TextColored(ImVec4(1,0,0,1), "Driver: FAIL");
    ImGui::SameLine();
    if (g_targetPID) ImGui::TextColored(ImVec4(0,1,0,1), "| PID: %u", g_targetPID);
    else             ImGui::TextColored(ImVec4(1,1,0,1), "| Not Attached");
    ImGui::SameLine();
    ImGui::Text("| %d players | %.0f fps", g_playerCount, ImGui::GetIO().Framerate);

    if (ImGui::Button("Reattach", ImVec2(-1, 0))) AttachToFortnite();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##tabs")) {

        if (ImGui::BeginTabItem("Visuals")) {
            ImGui::Checkbox("Enable ESP", &g_settings.enabled);
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

            if (g_settings.colorMode == ColorMode::STATIC) {
                ImGui::ColorEdit4("Box Color", g_settings.boxColor, ImGuiColorEditFlags_NoInputs);
            }
            if (g_settings.colorMode == ColorMode::DISTANCE_BASED) {
                ImGui::SliderFloat("Close (Red)", &g_settings.distClose, 10.f, 100.f);
                ImGui::SliderFloat("Mid (Yellow)", &g_settings.distMid, 50.f, 200.f);
                ImGui::SliderFloat("Far (Green)", &g_settings.distFar, 100.f, 500.f);
            }
            if (g_settings.colorMode == ColorMode::TEAM_BASED) {
                ImGui::ColorEdit4("Team Color", g_settings.teamColor, ImGuiColorEditFlags_NoInputs);
            }
            ImGui::Separator();
            ImGui::ColorEdit4("Health Bar", g_settings.healthColor, ImGuiColorEditFlags_NoInputs);
            ImGui::ColorEdit4("Shield Bar", g_settings.shieldColor, ImGuiColorEditFlags_NoInputs);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Info")) {
            ImGui::Checkbox("Player Names", &g_settings.showPlayerName);
            ImGui::Checkbox("Health / Shield Bars", &g_settings.showHealthBar);
            ImGui::Checkbox("Weapon Info", &g_settings.showWeaponInfo);
            ImGui::Checkbox("Status Indicators", &g_settings.showStatusIndicators);
            ImGui::Checkbox("Kill Count", &g_settings.showKillCount);
            ImGui::Separator();
            ImGui::SliderInt("Max Distance (m)", &g_settings.maxDistance, 50, 500);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Skeleton")) {
            ImGui::Checkbox("Show Local Skeleton", &g_settings.showLocalSkeleton);
            ImGui::Separator();
            ImGui::Checkbox("Gradient Color", &g_settings.skeletonGradient);
            ImGui::Checkbox("Glow Effect", &g_settings.skeletonGlow);
            ImGui::SliderFloat("Line Width", &g_settings.skeletonLineWidth, 0.5f, 4.0f);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Aimbot")) {
            ImGui::Checkbox("Enable Aimbot", &g_aim.enabled);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1,1,0,1), g_aim.masterEnabled ? "[P=ON]" : "[P=OFF]");
            ImGui::Separator();
            ImGui::SliderFloat("Smoothness", &g_aim.smooth, 0.01f, 0.50f, "%.2f");
            ImGui::SliderFloat("FOV (degrees)", &g_aim.fov, 1.0f, 90.0f, "%.0f");
            ImGui::SliderFloat("Stick Sensitivity", &g_aim.stickSensitivity, 0.1f, 1.0f, "%.2f");
            ImGui::SliderFloat("Stick Deadzone", &g_aim.stickDeadzone, 0.0f, 0.20f, "%.2f");
            ImGui::SliderFloat("Random Skip", &g_aim.randomSkip, 0.0f, 0.9f, "%.2f");
            const char* aimKeys = "Right Mouse\0Left Mouse\0Middle Mouse\0Side Button 1\0Side Button 2\0Shift\0Ctrl\0Alt\0Space\0Tab\0Caps Lock\0";
            int aimKeyIdx = 0;
            switch (g_aim.aimKey) {
                case VK_RBUTTON: aimKeyIdx = 0; break;
                case VK_LBUTTON: aimKeyIdx = 1; break;
                case VK_MBUTTON: aimKeyIdx = 2; break;
                case VK_XBUTTON1: aimKeyIdx = 3; break;
                case VK_XBUTTON2: aimKeyIdx = 4; break;
                case VK_SHIFT: aimKeyIdx = 5; break;
                case VK_CONTROL: aimKeyIdx = 6; break;
                case VK_MENU: aimKeyIdx = 7; break;
                case VK_SPACE: aimKeyIdx = 8; break;
                case VK_TAB: aimKeyIdx = 9; break;
                case VK_CAPITAL: aimKeyIdx = 10; break;
            }
            if (ImGui::Combo("Aim Key", &aimKeyIdx, aimKeys)) {
                switch (aimKeyIdx) {
                    case 0: g_aim.aimKey = VK_RBUTTON; break;
                    case 1: g_aim.aimKey = VK_LBUTTON; break;
                    case 2: g_aim.aimKey = VK_MBUTTON; break;
                    case 3: g_aim.aimKey = VK_XBUTTON1; break;
                    case 4: g_aim.aimKey = VK_XBUTTON2; break;
                    case 5: g_aim.aimKey = VK_SHIFT; break;
                    case 6: g_aim.aimKey = VK_CONTROL; break;
                    case 7: g_aim.aimKey = VK_MENU; break;
                    case 8: g_aim.aimKey = VK_SPACE; break;
                    case 9: g_aim.aimKey = VK_TAB; break;
                    case 10: g_aim.aimKey = VK_CAPITAL; break;
                }
            }
            ImGui::Checkbox("Auto Fire", &g_aim.autoFire);
            ImGui::Checkbox("Aim at Teammates", &g_aim.aimAtTeam);
            if (g_vigem.IsReady()) ImGui::TextColored(ImVec4(0,1,0,1), "ViGEm: Connected");
            else ImGui::TextColored(ImVec4(1,0,0,1), "ViGEm: Not Connected");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ============================================================
// Main
// ============================================================
int main()
{
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);

    printf("========================================\n");
    printf("  Fortnite ESP Box - v41.20\n");
    printf("========================================\n\n");

    g_screenWidth = GetSystemMetrics(SM_CXSCREEN);
    g_screenHeight = GetSystemMetrics(SM_CYSCREEN);
    printf("[+] Screen: %dx%d\n", g_screenWidth, g_screenHeight);

    g_driverReady = InitializeDriver();
    if (!g_driverReady) {
        printf("\n[-] Driver failed. Press Enter...\n");
        std::cin.get();
        return 1;
    }

    if (!AttachToFortnite())
        printf("[!] Fortnite not running. Use Reattach button later.\n");

    if (g_vigem.Init())
        printf("[+] ViGEm ready - aimbot uses virtual Xbox 360 controller\n");
    else
        printf("[-] ViGEm init failed - aimbot via controller disabled\n");

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
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't persist window layout / always use defaults
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
    style.AntiAliasedLines = false;
    style.AntiAliasedFill = false;
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
            LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
            if (showMenu) SetWindowLong(hwnd, GWL_EXSTYLE, style & ~WS_EX_TRANSPARENT);
            else          SetWindowLong(hwnd, GWL_EXSTYLE, style | WS_EX_TRANSPARENT);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        g_renderFrameIdx = g_readIdx.load();
        if (showMenu) RenderMenu();
        RenderESP();
        RunAimbot();

        ImGui::Render();
        const float cc[4] = { 0, 0, 0, 0 };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(0, 0);

        // Frame limiter: wait until target time reached from last frame
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - lastFrame.QuadPart) / perfFreq.QuadPart;
        if (elapsed < (1.0 / 60.0)) {
            DWORD sleepMs = (DWORD)(((1.0 / 60.0) - elapsed) * 1000.0);
            if (sleepMs > 0) Sleep(sleepMs);
        }
        QueryPerformanceCounter(&lastFrame);
    }

    g_espThreadRunning = false;
    if (g_espThread.joinable()) g_espThread.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (xhdr::g_dev != INVALID_HANDLE_VALUE) CloseHandle(xhdr::g_dev);
    if (xhdr::g_targetH) CloseHandle(xhdr::g_targetH);
    return 0;
}

// ============================================================
// DirectX helpers
// ============================================================
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 0, 1 };
    sd.Flags = 0;
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
