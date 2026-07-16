#pragma once
// ===========================================================================
// Offsets & Decryption - Fortnite UE5
//
// Build: ++Fortnite+Release-41.20-CL-XXXXXXXX-Windows
// Source: UObject Dump (2026-07-16)
// Decrypt: XOR 0x93F1FA5800000000 + _byteswap_uint64
// ===========================================================================

#include <cstdint>
#include <cstdlib>

namespace offsets {

namespace core
{
    constexpr uintptr_t GWorld                           = 0x19D0D570;
    constexpr uintptr_t gEngine                          = 0x19DE55F8;
    constexpr uintptr_t GameViewport                     = 0xB70;
    constexpr uintptr_t GameInstance                     = 0x190;    // UWorld::OwningGameInstance
    constexpr uintptr_t GameState                        = 0x130;    // UWorld::GameState
    constexpr uintptr_t RootComponent                    = 0x130;    // Actor::RootComponent
    constexpr uintptr_t ComponentToWorld                 = 0x1E0;    // SceneComponent (internal inline)
    constexpr uintptr_t BoneArray                        = 0x710;    // SkeletalMeshComponent::CachedBoneSpaceTransforms (TArray)
    constexpr uintptr_t BoneArray_cache                  = 0x720;    // SkeletalMeshComponent::CachedComponentSpaceTransforms
    constexpr uintptr_t PlayerArray                      = 0x238;    // GameStateBase::PlayerArray
    constexpr uintptr_t RelativeLocation                 = 0x11C;    // SceneComponent::RelativeLocation (FVector3d)
    constexpr uintptr_t FOV                              = 0x374;
    constexpr uintptr_t PersistentLevel                  = 0x30;     // UWorld::PersistentLevel
    constexpr uintptr_t Levels                           = 0x148;    // UWorld::Levels (TArray<ULevel*>)
    constexpr uintptr_t Actors                           = 0x38;     // ULevel::Actors (via LevelActorContainer ptr)
    constexpr uintptr_t LevelActorContainer              = 0xD8;     // ULevel::ActorCluster -> LevelActorContainer

    // View projection matrix (CachedViewInfo array in UWorld - may need fallback)
    constexpr uintptr_t CachedViewInfoRenderedLastFrame  = 0x1A0;

    // Camera Manager POV (via CameraCachePrivate at PlayerCameraManager+0x1A60, FTViewTarget)
    constexpr uintptr_t POV_Location    = 0x1A68;   // CamMgr + 0x1A60 + 0x08
    constexpr uintptr_t POV_Rotation    = 0x1A80;   // +0x18 from POV start
    constexpr uintptr_t POV_FOV         = 0x1A98;   // +0x30 from POV start

    // APlayerController
    constexpr uintptr_t CameraManager      = 0x2B8;    // PlayerController::PlayerCameraManager
    constexpr uintptr_t ControlRotation    = 0x288;    // Controller::ControlRotation
    constexpr uintptr_t AcknowledgedPawn   = 0x2A0;    // PlayerController::AcknowledgedPawn
}

namespace aimbot {
    constexpr float DEFAULT_SMOOTH     = 0.12f;
    constexpr float DEFAULT_FOV        = 15.0f;
    constexpr float DEFAULT_MAX_RANGE  = 300.0f;
    constexpr int   DEFAULT_AIM_KEY    = VK_RBUTTON;
    constexpr int   BONE_CHEST         = 2;
    constexpr int   BONE_HEAD          = 0;
}

namespace player
{
    constexpr uintptr_t Mesh              = 0x280;    // Character::Mesh (USkeletalMeshComponent*)
    constexpr uintptr_t LocalPlayers      = 0x38;     // GameInstance::LocalPlayers
    constexpr uintptr_t PlayerController  = 0x30;     // ULocalPlayer::PlayerController
    constexpr uintptr_t LocalPawn         = 0x718;    // FortPlayerController::MyFortPawn
    constexpr uintptr_t PawnPrivate       = 0x280;    // PlayerState::PawnPrivate
    constexpr uintptr_t PlayerState       = 0x240;    // Pawn::PlayerState
    constexpr uintptr_t TeamIndex         = 0xE68;    // FortPlayerStateAthena::TeamIndex (uint8)
    constexpr uintptr_t bIsDying          = 0x538;    // FortPawn::bIsDying (bitfield byte)
    constexpr uintptr_t bIsDBNO           = 0x552;    // FortPawn::bIsDBNO (bitfield byte)
    constexpr uintptr_t bIsABot           = 0x22A;    // PlayerState::bIsABot (bitfield byte)
    constexpr uintptr_t CurrentWeapon     = 0x5A0;    // FortPawn::CurrentWeapon
    constexpr uintptr_t KillScore         = 0xE7C;    // FortPlayerStateAthena::KillScore
    constexpr uintptr_t bIsSliding        = 0x723;    // *** needs verification (not in 41.20 dump) ***
    constexpr uintptr_t bIsSkydiving      = 0x11A5;   // FortPlayerPawn::bIsSkydiving (bitfield byte, bit 0)
    constexpr uintptr_t bIsCrouched       = 0x330;    // Character::bIsCrouched (bitfield byte)
}

namespace weapon
{
    constexpr uintptr_t WeaponData        = 0x378;    // FortWeapon::WeaponData (UFortWeaponItemDefinition*)
    constexpr uintptr_t AmmoCount         = 0x934;    // FortWeapon::AmmoCount
    constexpr uintptr_t bIsReloading      = 0x2B1;    // FortWeapon::bIsReloadingWeapon (bitfield byte)
    constexpr uintptr_t ItemName          = 0x70;     // FortItemDefinition::DisplayName (FText)
}

namespace uworld {
    constexpr uint64_t kMask = 0x93F1FA5800000000ULL;

    inline uintptr_t decrypt(uint64_t encrypted) {
        if (!encrypted || encrypted == kMask) return 0;
        return static_cast<uintptr_t>(_byteswap_uint64(encrypted ^ kMask));
    }
}

} // namespace offsets
