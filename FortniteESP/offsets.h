#pragma once
// ===========================================================================
// Offsets & Decryption - Fortnite UE5
//
// Build: ++Fortnite+Release-41.20-CL-55550516-Windows  (July 16, 2026)
// Source: cheatoffsets.com (2026-07-16)
//
// UWorld decryption may have changed from v41.10 - verify with memory dump.
// ===========================================================================

#include <cstdint>
#include <cstdlib>   // for _byteswap_uint64

namespace offsets {

namespace core
{
    constexpr uintptr_t UWORLD                           = 0x1B2439A0;  // 41.20
    constexpr uintptr_t gEngine                          = 0x1B245388;  // cheatoffsets 41.20
    constexpr uintptr_t GameViewport                     = 0xB70;       // from GEngine (UGameEngine)
    constexpr uintptr_t GameInstance                     = 0x248;       // 41.10, verify
    constexpr uintptr_t ViewportClient                   = 0x78;        // UGameViewportClient::World
    constexpr uintptr_t GameState                        = 0x1D8;       // 41.20 (was 0x1D0)
    constexpr uintptr_t RootComponent                    = 0x1B0;       // confirmed (AActor)
    constexpr uintptr_t ComponentToWorld                 = 0x1E0;       // 41.10, verify (USceneComponent)
    constexpr uintptr_t BoneArray                        = 0x650;       // 41.10, verify (USkeletalMeshComponent)
    constexpr uintptr_t BoneArray_cache                  = 0x660;       // 41.10, verify
    constexpr uintptr_t PlayerArray                      = 0x288;       // 41.10, verify (GameState)
    constexpr uintptr_t RelativeLocation                 = 0x140;       // confirmed (USceneComponent)
    constexpr uintptr_t LocationPointer                  = 0x178;       // 41.10, verify
    constexpr uintptr_t RotationPointer                  = 0x188;       // 41.10, verify
    constexpr uintptr_t FOV                              = 0x374;       // 41.10, verify
    constexpr uintptr_t PersistentLevel                  = 0x38;        // confirmed (UWorld)
    constexpr uintptr_t Levels                           = 0x1E8;       // 41.10, verify
    constexpr uintptr_t Actors                           = 0x38;        // 41.10, verify (ULevel)
    constexpr uintptr_t LastRenderTime                   = 0x338;       // 41.10, verify

    // Camera (ViewProjectionMatrix from CachedViewInfoRenderedLastFrame)
    // Not a UPROPERTY (hidden from reflection) - UC: DefaultPhysicsVolume(0x168) + 0x20 = 0x188
    constexpr uintptr_t CachedViewInfoRenderedLastFrame  = 0x188;       // confirmed same

    // Camera manager POV (live render camera)
    constexpr uintptr_t POV_Location    = 0x15A0;  // CameraCachePrivate(0x1590)+POV(0x10)+Location(0x0) = 0x15A0
    constexpr uintptr_t POV_Rotation    = 0x15B8;  // + Rotation(0x18) = 0x15B8
    constexpr uintptr_t POV_FOV         = 0x15D0;  // + FOV(0x30) = 0x15D0
}

namespace aimbot {
    inline constexpr float DEFAULT_SMOOTH     = 0.12f;
    inline constexpr float DEFAULT_FOV        = 15.0f;
    inline constexpr float DEFAULT_MAX_RANGE  = 300.0f;
    inline constexpr int   DEFAULT_AIM_KEY    = VK_RBUTTON;
    inline constexpr int   BONE_CHEST         = 2;
    inline constexpr int   BONE_HEAD          = 0;
}

namespace player
{
    constexpr uintptr_t Mesh              = 0x2F0;       // confirmed (ACharacter)
    constexpr uintptr_t LocalPlayers      = 0x38;        // confirmed (UGameInstance)
    constexpr uintptr_t PlayerController  = 0x30;        // 41.10, verify (ULocalPlayer)
    constexpr uintptr_t LocalPawn         = 0x318;       // confirmed (APlayerController::AcknowledgedPawn)
    constexpr uintptr_t PawnPrivate       = 0x2E8;       // confirmed (APlayerState)
    constexpr uintptr_t PlayerState       = 0x290;       // confirmed (APawn)
    constexpr uintptr_t TeamIndex         = 0xF11;       // 41.10, verify (Fort-specific)
    constexpr uintptr_t bIsDying          = 0x720;       // 41.10, verify (Fort-specific)
    constexpr uintptr_t bIsDBNO           = 0x849;       // 41.10, verify (Fort-specific)
    constexpr uintptr_t bIsABot           = 0x27A;       // confirmed (APlayerState)
    constexpr uintptr_t CurrentWeapon     = 0x998;       // 41.10, verify (Fort-specific)
    constexpr uintptr_t KillScore         = 0xF48;       // 41.20 (was 0xF28)
    constexpr uintptr_t bIsSliding        = 0x723;       // 41.10, verify (Fort-specific)
    constexpr uintptr_t bIsSkydiving      = 0x216F;      // 41.10, verify (Fort-specific)
    constexpr uintptr_t bIsCrouched       = 0x430;       // confirmed (ACharacter)
}

namespace weapon
{
    constexpr uintptr_t WeaponData        = 0x630;       // 41.10, verify (Fort-specific)
    constexpr uintptr_t AmmoCount         = 0x10A4;      // 41.10, verify (Fort-specific)
    constexpr uintptr_t bIsReloading      = 0x381;       // 41.20 (was 0x379)
    constexpr uintptr_t ItemName          = 0x38;        // 41.10, verify (Fort-specific)
}

// UWorld decryption (v41.10 - byteswap-xor)
namespace uworld {
    constexpr uint64_t kMask = 0x93F1FA5800000000ULL;

    inline uintptr_t decrypt(uint64_t encrypted) {
        if (!encrypted || encrypted == kMask) return 0;
        return static_cast<uintptr_t>(_byteswap_uint64(encrypted ^ kMask));
    }
}

} // namespace offsets

