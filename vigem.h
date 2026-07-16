#pragma once
#include <Windows.h>

// XUSB_REPORT for Xbox 360 controller state
#pragma pack(push, 1)
struct XUSB_REPORT {
    USHORT wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
#pragma pack(pop)

// Button flags
#define XUSB_GAMEPAD_DPAD_UP        0x0001
#define XUSB_GAMEPAD_DPAD_DOWN      0x0002
#define XUSB_GAMEPAD_DPAD_LEFT      0x0004
#define XUSB_GAMEPAD_DPAD_RIGHT     0x0008
#define XUSB_GAMEPAD_START          0x0010
#define XUSB_GAMEPAD_BACK           0x0020
#define XUSB_GAMEPAD_LEFT_THUMB     0x0040
#define XUSB_GAMEPAD_RIGHT_THUMB    0x0080
#define XUSB_GAMEPAD_LEFT_SHOULDER  0x0100
#define XUSB_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XUSB_GAMEPAD_GUIDE          0x0400
#define XUSB_GAMEPAD_A              0x1000
#define XUSB_GAMEPAD_B              0x2000
#define XUSB_GAMEPAD_X              0x4000
#define XUSB_GAMEPAD_Y              0x8000

// ViGEm error codes
typedef LONG VIGEM_ERROR;
#define VIGEM_SUCCESS           0x20000000
#define VIGEM_ERROR_BUS_NOT_FOUND  0xE0000001
#define VIGEM_ERROR_NO_FREE_SLOT   0xE0000002
#define VIGEM_ERROR_INVALID_TARGET 0xE0000003
#define VIGEM_ERROR_REMOVAL_FAILED 0xE0000004
#define VIGEM_ERROR_ALREADY_CONNECTED 0xE0000005
#define VIGEM_ERROR_BUS_ALREADY_OCCUPIED 0xE0000006
#define VIGEM_ERROR_BUS_ACCESS_FAILED 0xE0000007
#define VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED 0xE0000008
#define VIGEM_ERROR_CALLBACK_NOT_FOUND 0xE0000009
#define VIGEM_ERROR_BUS_VERSION_MISMATCH 0xE000000A
#define VIGEM_ERROR_TARGET_UNINITIALIZED 0xE000000B
#define VIGEM_ERROR_ALREADY_SUBSCRIBED 0xE000000C
#define VIGEM_ERROR_INSUFFICIENT_SIZE 0xE000000D
#define VIGEM_ERROR_NOT_SUPPORTED 0xE000000E

// Opaque handle types
struct VIGEM_CLIENT { UINT32 id; };
struct VIGEM_TARGET { UINT32 id; };
typedef struct VIGEM_CLIENT *PVIGEM_CLIENT;
typedef struct VIGEM_TARGET *PVIGEM_TARGET;

class ViGEmManager {
public:
    ViGEmManager() : m_dll(NULL), m_client(NULL), m_target(NULL), m_initialized(false) {}

    ~ViGEmManager() { Shutdown(); }

    bool Init() {
        if (m_initialized) return true;

        m_dll = LoadLibraryW(L"ViGEmClient.dll");
        if (!m_dll) {
            // Try loading from system path
            m_dll = LoadLibraryW(L"ViGEmClient.dll");
            if (!m_dll) return false;
        }

        #define LOAD_FN(name) \
            m_##name = (decltype(m_##name))GetProcAddress(m_dll, "vigem_" #name); \
            if (!m_##name) { printf("[-] vigem_%s not found\n", #name); Shutdown(); return false; }

        LOAD_FN(alloc);
        LOAD_FN(connect);
        LOAD_FN(disconnect);
        LOAD_FN(free);
        LOAD_FN(target_add);
        LOAD_FN(target_remove);
        LOAD_FN(target_x360_alloc);
        LOAD_FN(target_x360_update);
        LOAD_FN(target_free);

        m_client = m_alloc();
        if (!m_client) {
            printf("[-] vigem_alloc failed\n");
            Shutdown();
            return false;
        }

        VIGEM_ERROR err = m_connect(m_client);
        if (err != VIGEM_SUCCESS) {
            printf("[-] vigem_connect failed: 0x%08X\n", (unsigned)err);
            Shutdown();
            return false;
        }

        m_target = m_target_x360_alloc();
        if (!m_target) {
            printf("[-] vigem_target_x360_alloc failed\n");
            Shutdown();
            return false;
        }

        err = m_target_add(m_client, m_target);
        if (err != VIGEM_SUCCESS) {
            printf("[-] vigem_target_add failed: 0x%08X\n", (unsigned)err);
            Shutdown();
            return false;
        }

        // Zero out initial state
        XUSB_REPORT report = {};
        m_target_x360_update(m_client, m_target, report);

        m_initialized = true;
        printf("[+] ViGEm virtual Xbox 360 controller created!\n");
        return true;
    }

    void Shutdown() {
        if (m_target && m_client) {
            m_target_remove(m_client, m_target);
            m_target_free(m_target);
            m_target = NULL;
        }
        if (m_client) {
            if (m_connect) m_disconnect(m_client);
            m_free(m_client);
            m_client = NULL;
        }
        if (m_dll) {
            FreeLibrary(m_dll);
            m_dll = NULL;
        }
        m_initialized = false;
    }

    bool Update(const XUSB_REPORT& report) {
        if (!m_initialized || !m_client || !m_target) return false;
        VIGEM_ERROR err = m_target_x360_update(m_client, m_target, report);
        return err == VIGEM_SUCCESS;
    }

    bool IsReady() const { return m_initialized; }

private:
    HMODULE m_dll;
    PVIGEM_CLIENT m_client;
    PVIGEM_TARGET m_target;
    bool m_initialized;

    // Function pointers
    PVIGEM_CLIENT (__cdecl *m_alloc)() = nullptr;
    VIGEM_ERROR (__cdecl *m_connect)(PVIGEM_CLIENT) = nullptr;
    void (__cdecl *m_disconnect)(PVIGEM_CLIENT) = nullptr;
    void (__cdecl *m_free)(PVIGEM_CLIENT) = nullptr;
    VIGEM_ERROR (__cdecl *m_target_add)(PVIGEM_CLIENT, PVIGEM_TARGET) = nullptr;
    VIGEM_ERROR (__cdecl *m_target_remove)(PVIGEM_CLIENT, PVIGEM_TARGET) = nullptr;
    PVIGEM_TARGET (__cdecl *m_target_x360_alloc)() = nullptr;
    VIGEM_ERROR (__cdecl *m_target_x360_update)(PVIGEM_CLIENT, PVIGEM_TARGET, XUSB_REPORT) = nullptr;
    void (__cdecl *m_target_free)(PVIGEM_TARGET) = nullptr;
};
