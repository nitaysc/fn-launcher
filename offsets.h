#pragma once
// ===========================================================================
// Offsets & Decryption - Fortnite UE5
//
// Build: ++Fortnite+Release-41.10-CL-55227503-Windows  (June 25, 2026)
// Source: UC forum post by "underscores" + decrypt by "gulohn" (June 26, 2026)
//         PlayerAimOffset correction by "CapsuleMesh"
//
// Decrypt formula changed in this patch - was rotate-mul-add, is now byteswap-xor.
// ===========================================================================

#include <cstdint>
#include <cstdlib>   // for _byteswap_uint64

namespace offsets {

namespace core
{
    constexpr uintptr_t UWORLD                           = 0x19DE3C10;
    constexpr uintptr_t gEngine                          = 0x19DE55F8;
    constexpr uintptr_t GameViewport                     = 0xB70;
    constexpr uintptr_t GameInstance                     = 0x248;
    constexpr uintptr_t GameState                        = 0x1D0;
    constexpr uintptr_t RootComponent                    = 0x1B0;
    constexpr uintptr_t ComponentToWorld                 = 0x1E0;
    constexpr uintptr_t BoneArray                        = 0x650;
    constexpr uintptr_t BoneArray_cache                  = 0x660;
    constexpr uintptr_t PlayerArray                      = 0x288;
    constexpr uintptr_t RelativeLocation                 = 0x140;
    constexpr uintptr_t LocationPointer                  = 0x178;
    constexpr uintptr_t RotationPointer                  = 0x188;
    constexpr uintptr_t FOV                              = 0x374;
    constexpr uintptr_t PersistentLevel                  = 0x38;
    constexpr uintptr_t Levels                           = 0x1E8;
    constexpr uintptr_t Actors                           = 0x38;
    constexpr uintptr_t LastRenderTime                   = 0x338;

    // Camera (ViewProjectionMatrix from CachedViewInfoRenderedLastFrame)
    // Not a UPROPERTY (hidden from reflection) - UC: DefaultPhysicsVolume(0x168) + 0x20 = 0x188
    constexpr uintptr_t CachedViewInfoRenderedLastFrame  = 0x188;

    // Camera manager POV (live render camera)
    constexpr uintptr_t POV_Location    = 0x15A0;
    constexpr uintptr_t POV_Rotation    = 0x15B8;
    constexpr uintptr_t POV_FOV         = 0x15D0;

    // APlayerController offsets
    constexpr uintptr_t ControlRotation_1 = 0x530;  // common UE5
    constexpr uintptr_t ControlRotation_2 = 0x528;  // alternative
    constexpr uintptr_t ControlRotation_3 = 0x540;  // alternative
    constexpr uintptr_t ControlRotation_4 = 0x4B0;  // older
    constexpr uintptr_t ControlRotation_5 = 0x300;  // base AController
    constexpr uintptr_t ControlRotation_6 = 0x308;  // base AController alt
    constexpr uintptr_t CameraManager   = 0x478;  // APlayerCameraManager*
}

namespace aimbot {
    // Default settings
    constexpr float DEFAULT_SMOOTH     = 0.12f;
    constexpr float DEFAULT_FOV        = 15.0f;
    constexpr float DEFAULT_MAX_RANGE  = 300.0f;
    constexpr int   DEFAULT_AIM_KEY    = VK_RBUTTON; // Right mouse button
    constexpr int   BONE_CHEST         = 2;
    constexpr int   BONE_HEAD          = 0;
}

namespace player
{
    constexpr uintptr_t Mesh              = 0x2F0;
    constexpr uintptr_t LocalPlayers      = 0x38;
    constexpr uintptr_t PlayerController  = 0x30;
    constexpr uintptr_t LocalPawn         = 0x318;
    constexpr uintptr_t PawnPrivate       = 0x2E8;
    constexpr uintptr_t PlayerState       = 0x290;
    constexpr uintptr_t TeamIndex         = 0xF11;
    constexpr uintptr_t bIsDying          = 0x720;
    constexpr uintptr_t bIsDBNO           = 0x849;
    constexpr uintptr_t bIsABot           = 0x27A;
    constexpr uintptr_t CurrentWeapon     = 0x998;
    constexpr uintptr_t KillScore         = 0xF28;
    constexpr uintptr_t bIsSliding        = 0x723;
    constexpr uintptr_t bIsSkydiving      = 0x216F;
    constexpr uintptr_t bIsCrouched       = 0x430;
}

namespace weapon
{
    constexpr uintptr_t WeaponData        = 0x630;
    constexpr uintptr_t AmmoCount         = 0x10A4;
    constexpr uintptr_t bIsReloading      = 0x379;
    constexpr uintptr_t ItemName          = 0x38;
}

// UWorld decryption (v41.10 CL-55227503)
namespace uworld {
    constexpr uint64_t kMask = 0x93F1FA5800000000ULL;

    inline uintptr_t decrypt(uint64_t encrypted) {
        if (!encrypted || encrypted == kMask) return 0;
        return static_cast<uintptr_t>(_byteswap_uint64(encrypted ^ kMask));
    }
}

} // namespace offsets

