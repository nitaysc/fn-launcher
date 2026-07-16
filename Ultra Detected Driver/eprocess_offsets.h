#pragma once
// eprocess_offsets.h
// Dynamically resolves EPROCESS field offsets by parsing Ps* export function
// bodies from ntoskrnl.exe on disk.  No hardcoded per-build tables needed.
// Works on all Windows 10/11 x64 builds, Intel and AMD.
//
// Usage:
//   EprocessOffsets off;
//   if (!NtoskrnlParser::Resolve(off)) { /* fail */ }
//   off.Dump();

#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Result structure
// ---------------------------------------------------------------------------

struct EprocessOffsets {
    uint32_t DirectoryTableBase;            // constant 0x28 on all x64 Windows
    uint32_t UniqueProcessId;               // PsGetProcessId
    uint32_t ActiveProcessLinks;            // = UniqueProcessId + 8
    uint32_t ImageFileName;                 // PsGetProcessImageFileName
    uint32_t Peb;                           // PsGetProcessPeb
    uint32_t Job;                           // PsGetProcessJob
    uint32_t Token;                         // = Job - 0x58
    uint32_t ExitStatus;                    // PsGetProcessExitStatus
    uint32_t VadRoot;                       // = ExitStatus + 0x4
    uint32_t Protection;                    // = ExitStatus + 0xA6
    uint32_t SectionBaseAddress;            // PsGetProcessSectionBaseAddress
    uint32_t Cookie;                        // = SectionBaseAddress + 0x8
    uint32_t InheritedFromUniqueProcessId;  // PsGetProcessInheritedFromUniqueProcessId
    uint32_t PsInitialSystemProcessRVA;     // RVA of PsInitialSystemProcess global in ntoskrnl
    bool     valid;

    void Dump() const {
        if (!valid) { printf("[-] EprocessOffsets: not valid\n"); return; }
        printf("[+] EPROCESS offsets (parsed from ntoskrnl.exe):\n");
        printf("    DirectoryTableBase            = 0x%03X\n", DirectoryTableBase);
        printf("    UniqueProcessId               = 0x%03X\n", UniqueProcessId);
        printf("    ActiveProcessLinks            = 0x%03X\n", ActiveProcessLinks);
        printf("    ImageFileName                 = 0x%03X\n", ImageFileName);
        printf("    Peb                           = 0x%03X\n", Peb);
        printf("    Job                           = 0x%03X\n", Job);
        printf("    Token                         = 0x%03X\n", Token);
        printf("    ExitStatus                    = 0x%03X\n", ExitStatus);
        printf("    VadRoot                       = 0x%03X\n", VadRoot);
        printf("    Protection                    = 0x%03X\n", Protection);
        printf("    SectionBaseAddress            = 0x%03X\n", SectionBaseAddress);
        printf("    Cookie                        = 0x%03X\n", Cookie);
        printf("    InheritedFromUniqueProcessId  = 0x%03X\n", InheritedFromUniqueProcessId);
        printf("    PsInitialSystemProcessRVA     = 0x%08X\n", PsInitialSystemProcessRVA);
    }
};

// ---------------------------------------------------------------------------
// Parser implementation
// ---------------------------------------------------------------------------

namespace NtoskrnlParser {
namespace detail {

static uint32_t RvaToFileOffset(const uint8_t* pe, uint32_t rva) {
    auto* dos = (const IMAGE_DOS_HEADER*)pe;
    auto* nt  = (const IMAGE_NT_HEADERS64*)(pe + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (rva >= sec->VirtualAddress &&
            rva <  sec->VirtualAddress + sec->Misc.VirtualSize)
            return (rva - sec->VirtualAddress) + sec->PointerToRawData;
    }
    return 0;
}

// Scan first maxBytes of a function for any [rcx + imm] memory access.
// These Ps* functions always access [rcx+offset] as their first meaningful op.
static uint32_t ExtractRcxOffset(const uint8_t* fn, size_t maxBytes = 96) {
    for (size_t i = 0; i + 7 <= maxBytes; i++) {
        const uint8_t* b = fn + i;

        // Follow a short/near JMP once (thunk pattern)
        if (b[0] == 0xEB) { fn = b + 2 + (int8_t)b[1]; i = 0; maxBytes = 96; continue; }
        if (b[0] == 0xE9) { int32_t rel; memcpy(&rel, b+1, 4); fn = b + 5 + rel; i = 0; maxBytes = 96; continue; }

        // REX.W  MOV/LEA/MOVSXD  rax, [rcx + imm32]
        // ModRM 0x81 = Mod:10 Reg:RAX(0) RM:RCX(1)
        if (b[0] == 0x48 &&
            (b[1] == 0x8B || b[1] == 0x8D || b[1] == 0x63) &&
            b[2] == 0x81) {
            uint32_t off; memcpy(&off, b + 3, 4);
            if (off >= 0x28 && off < 0x3000) return off;
        }

        // REX.W  MOV/LEA/MOVSXD  rax, [rcx + imm8]
        // ModRM 0x41 = Mod:01 Reg:RAX(0) RM:RCX(1)
        if (b[0] == 0x48 &&
            (b[1] == 0x8B || b[1] == 0x8D || b[1] == 0x63) &&
            b[2] == 0x41) {
            uint8_t off = b[3];
            if (off >= 0x28) return (uint32_t)off;
        }

        // MOV  eax, [rcx + imm32]  (no REX)
        if (b[0] == 0x8B && b[1] == 0x81) {
            uint32_t off; memcpy(&off, b + 2, 4);
            if (off >= 0x28 && off < 0x3000) return off;
        }

        // MOV  eax, [rcx + imm8]  (no REX)
        if (b[0] == 0x8B && b[1] == 0x41) {
            uint8_t off = b[2];
            if (off >= 0x28) return (uint32_t)off;
        }

        // MOVZX  eax, byte/word ptr [rcx + imm32]
        if (b[0] == 0x0F && (b[1] == 0xB6 || b[1] == 0xB7) && b[2] == 0x81) {
            uint32_t off; memcpy(&off, b + 3, 4);
            if (off >= 0x28 && off < 0x3000) return off;
        }

        // MOVZX  eax, byte/word ptr [rcx + imm8]
        if (b[0] == 0x0F && (b[1] == 0xB6 || b[1] == 0xB7) && b[2] == 0x41) {
            uint8_t off = b[3];
            if (off >= 0x28) return (uint32_t)off;
        }
    }
    return 0;
}

static const uint8_t* GetExportFunc(const uint8_t* pe, size_t fileSize,
                                     const char* name) {
    auto* dos = (const IMAGE_DOS_HEADER*)pe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = (const IMAGE_NT_HEADERS64*)(pe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    auto& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!ed.VirtualAddress) return nullptr;

    uint32_t eFO = RvaToFileOffset(pe, ed.VirtualAddress);
    if (!eFO || eFO + sizeof(IMAGE_EXPORT_DIRECTORY) > fileSize) return nullptr;

    auto* exp   = (const IMAGE_EXPORT_DIRECTORY*)(pe + eFO);
    auto* names = (const DWORD*)(pe + RvaToFileOffset(pe, exp->AddressOfNames));
    auto* ords  = (const WORD* )(pe + RvaToFileOffset(pe, exp->AddressOfNameOrdinals));
    auto* funcs = (const DWORD*)(pe + RvaToFileOffset(pe, exp->AddressOfFunctions));

    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        uint32_t nFO = RvaToFileOffset(pe, names[i]);
        if (!nFO || nFO >= (uint32_t)fileSize) continue;
        if (strcmp((const char*)(pe + nFO), name) != 0) continue;

        DWORD funcRva = funcs[ords[i]];
        // Skip forwarded exports
        if (funcRva >= ed.VirtualAddress && funcRva < ed.VirtualAddress + ed.Size)
            return nullptr;

        uint32_t fFO = RvaToFileOffset(pe, funcRva);
        if (!fFO || fFO >= (uint32_t)fileSize) return nullptr;
        return pe + fFO;
    }
    return nullptr;
}

// Returns the RVA of a named export (data or function) without scanning the body.
static uint32_t GetExportDataRVA(const uint8_t* pe, size_t fileSize, const char* name)
{
    auto* dos = (const IMAGE_DOS_HEADER*)pe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = (const IMAGE_NT_HEADERS64*)(pe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!ed.VirtualAddress) return 0;

    uint32_t eFO = RvaToFileOffset(pe, ed.VirtualAddress);
    if (!eFO || eFO + sizeof(IMAGE_EXPORT_DIRECTORY) > fileSize) return 0;

    auto* exp   = (const IMAGE_EXPORT_DIRECTORY*)(pe + eFO);
    auto* names = (const DWORD*)(pe + RvaToFileOffset(pe, exp->AddressOfNames));
    auto* ords  = (const WORD* )(pe + RvaToFileOffset(pe, exp->AddressOfNameOrdinals));
    auto* funcs = (const DWORD*)(pe + RvaToFileOffset(pe, exp->AddressOfFunctions));

    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        uint32_t nFO = RvaToFileOffset(pe, names[i]);
        if (!nFO || nFO >= (uint32_t)fileSize) continue;
        if (strcmp((const char*)(pe + nFO), name) != 0) continue;
        return funcs[ords[i]];  // raw RVA — valid for both code and data exports
    }
    return 0;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static bool Resolve(EprocessOffsets& out) {
    out        = {};
    out.valid  = false;
    out.DirectoryTableBase = 0x28; // invariant on all x64 Windows

    char sysDir[MAX_PATH] = {};
    GetSystemDirectoryA(sysDir, sizeof(sysDir));
    std::string path = std::string(sysDir) + "\\ntoskrnl.exe";

    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("[-] NtoskrnlParser: cannot open %s (err=%lu)\n", path.c_str(), GetLastError());
        return false;
    }

    DWORD sz = GetFileSize(hFile, nullptr);
    std::vector<uint8_t> buf(sz);
    DWORD rd = 0;
    ReadFile(hFile, buf.data(), sz, &rd, nullptr);
    CloseHandle(hFile);

    if (rd != sz) { printf("[-] NtoskrnlParser: short read\n"); return false; }

    const uint8_t* pe = buf.data();

    // Exports to parse directly
    struct Entry { const char* sym; uint32_t* dst; };
    Entry table[] = {
        { "PsGetProcessId",                          &out.UniqueProcessId              },
        { "PsGetProcessImageFileName",               &out.ImageFileName                },
        { "PsGetProcessPeb",                         &out.Peb                          },
        { "PsGetProcessJob",                         &out.Job                          },
        { "PsGetProcessExitStatus",                  &out.ExitStatus                   },
        { "PsGetProcessSectionBaseAddress",          &out.SectionBaseAddress           },
        { "PsGetProcessInheritedFromUniqueProcessId",&out.InheritedFromUniqueProcessId },
    };

    bool allOk = true;
    for (auto& e : table) {
        const uint8_t* fn = detail::GetExportFunc(pe, sz, e.sym);
        if (fn) *e.dst = detail::ExtractRcxOffset(fn);
        if (!*e.dst) {
            printf("[-] NtoskrnlParser: could not resolve %s\n", e.sym);
            allOk = false;
        }
    }

    // Derived offsets — fixed structural deltas invariant across Win10/11 builds
    if (out.UniqueProcessId)    out.ActiveProcessLinks = out.UniqueProcessId + 8;
    if (out.Job)                out.Token              = out.Job - 0x58;
    if (out.ExitStatus)         out.VadRoot            = (out.ExitStatus + 4 + 7) & ~7u;
    if (out.ExitStatus)         out.Protection         = out.ExitStatus + 0xA6;
    if (out.SectionBaseAddress) out.Cookie             = out.SectionBaseAddress + 0x8;

    // PsInitialSystemProcess: data export RVA — used to find System EPROCESS via ppa_x64
    out.PsInitialSystemProcessRVA = detail::GetExportDataRVA(pe, sz, "PsInitialSystemProcess");
    if (!out.PsInitialSystemProcessRVA)
        printf("[-] NtoskrnlParser: could not resolve PsInitialSystemProcess RVA\n");

    out.valid = allOk;
    return allOk;
}

} // namespace NtoskrnlParser
