#pragma once
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <psapi.h>
#include "eprocess_offsets.h"

#define DRV_LOG(fmt, ...) printf("[DRV] " fmt "\n", ##__VA_ARGS__)
#define XDRV_OK(fmt, ...) printf("[DRV OK] " fmt "\n", ##__VA_ARGS__)
#define DRV_ERR(fmt, ...) printf("[DRV ERR] " fmt "\n", ##__VA_ARGS__)

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// xhunter1.sys protocol internals
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

namespace xhdr {

static constexpr DWORD CMD_BUF_SIZE         = 624;
static constexpr DWORD CMD_MAGIC            = 0x345821ABu;
static constexpr DWORD RSP_MAGIC            = 0x12121012u;
static constexpr DWORD CMD_REGISTER         = 777;
static constexpr DWORD CMD_SET_FLAGS        = 775;
static constexpr DWORD CMD_OPEN_PROCESS     = 785;  // open handle with arbitrary access
static constexpr DWORD CMD_READ_PROCESS_MEM = 787;
static constexpr DWORD CMD_READ_KERNEL_MEM  = 788;
static constexpr DWORD CMD_INIT_INJECT      = 801;
static constexpr DWORD CMD_INJECT_EXEC      = 815;

#pragma pack(push, 1)
struct CmdBuffer {
	DWORD  size;
	DWORD  magic;
	DWORD  nonce;
	DWORD  cmd;
	UINT64 rspPtr;
	BYTE   params[600];
};
struct RspBuffer {
	DWORD  size;
	DWORD  magic;
	DWORD  nonceInv;
	DWORD  status;
	BYTE   data[746];
};
#pragma pack(pop)

static_assert(sizeof(CmdBuffer) == CMD_BUF_SIZE, "CmdBuffer must be 624 bytes");

static HANDLE          g_dev        = INVALID_HANDLE_VALUE;
static EprocessOffsets g_off        = {};
static bool            g_injectReady= false;
static UINT64          g_ntBase     = 0;
static UINT64          g_sysEproc   = 0;
static UINT64          g_ntAllocVm  = 0;  // NtAllocateVirtualMemory addr (same in every process)
static HANDLE          g_targetH    = nullptr;  // Cached process handle from cmd 785
static DWORD           g_targetPid  = 0;
static __declspec(align(16)) RspBuffer g_rsp = {};

static const BYTE kWriteShellcode[] = {
	0x56,                               // push rsi
	0x57,                               // push rdi
	0x53,                               // push rbx
	0x48, 0x83, 0xEC, 0x28,            // sub  rsp, 0x28
	0x48, 0x89, 0xCB,                  // mov  rbx, rcx
	0x48, 0x8D, 0x73, 0x30,            // lea  rsi, [rbx+0x30]
	0x48, 0x8B, 0x3E,                  // mov  rdi, [rsi]       ; destVA
	0x48, 0x85, 0xFF,                  // test rdi, rdi
	0x74, 0x0E,                        // jz   .done
	0x48, 0x83, 0xC6, 0x08,            // add  rsi, 8
	0x8B, 0x0E,                        // mov  ecx, [rsi]       ; len
	0x48, 0x83, 0xC6, 0x04,            // add  rsi, 4           ; -> data
	0xF3, 0xA4,                        // rep  movsb
	0xEB, 0xEA,                        // jmp  .loop
	0x48, 0x83, 0xC4, 0x28,            // add  rsp, 0x28
	0x5B,                               // pop  rbx
	0x5F,                               // pop  rdi
	0x5E,                               // pop  rsi
	0x31, 0xC0,                        // xor  eax, eax
	0xC3                                // ret
};

// READ shellcode: calls NtAllocateVirtualMemory in target process to create
// a SEPARATE buffer for dataPtr (driver's MDL-map requires this â€” pointing
// inside the existing shellcode page fails MmProbeAndLockPages). Then copies
// game memory into that buffer and sets up the readback header.
// Layout:
//   [0x000] shellcode
//   [0x100] descriptor: QWORD srcVA, DWORD size
//   [0x180] QWORD NtAllocateVirtualMemory address (filled by caller)
//   [0x190] QWORD BaseAddress (output of NtAllocateVM)
//   [0x198] QWORD RegionSize (input/output of NtAllocateVM)
//   [0x4094] readback header (status:4, dim1:4, dim2:4, ..., dataPtr@0x40B0)
static const BYTE kReadShellcode[] = {
	// Prologue â€” align stack for x64 call
	0x55,                                     // push rbp
	0x41, 0x56,                              // push r14
	0x48, 0x83, 0xEC, 0x38,                  // sub  rsp, 0x38         ; shadow+2 args+align
	0x49, 0x89, 0xCE,                        // mov  r14, rcx          ; r14 = page base
	// Set BaseAddress = 0
	0x31, 0xC0,                              // xor  eax, eax
	0x49, 0x89, 0x86, 0x90, 0x01, 0x00, 0x00, // mov  [r14+0x190], rax
	// RegionSize = size (from descriptor)
	0x41, 0x8B, 0x86, 0x08, 0x01, 0x00, 0x00, // mov  eax, [r14+0x108]
	0x49, 0x89, 0x86, 0x98, 0x01, 0x00, 0x00, // mov  [r14+0x198], rax (zero-extended)
	// NtAllocateVirtualMemory(-1, &BaseAddress, 0, &RegionSize, MEM_COMMIT|RESERVE, RW)
	0x48, 0xC7, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF, // mov  rcx, -1
	0x49, 0x8D, 0x96, 0x90, 0x01, 0x00, 0x00, // lea  rdx, [r14+0x190]
	0x4D, 0x31, 0xC0,                        // xor  r8, r8
	0x4D, 0x8D, 0x8E, 0x98, 0x01, 0x00, 0x00, // lea  r9, [r14+0x198]
	0xC7, 0x44, 0x24, 0x20, 0x00, 0x30, 0x00, 0x00, // mov [rsp+0x20], 0x3000
	0xC7, 0x44, 0x24, 0x28, 0x04, 0x00, 0x00, 0x00, // mov [rsp+0x28], 4
	0x49, 0x8B, 0x86, 0x80, 0x01, 0x00, 0x00, // mov  rax, [r14+0x180]  ; NtAllocateVM ptr
	0xFF, 0xD0,                              // call rax
	0x85, 0xC0,                              // test eax, eax
	0x75, 0x4F,                              // jnz  .fail (+79)
	// Copy srcVA -> allocated buffer
	0x49, 0x8B, 0xB6, 0x00, 0x01, 0x00, 0x00, // mov  rsi, [r14+0x100]  ; srcVA
	0x41, 0x8B, 0x8E, 0x08, 0x01, 0x00, 0x00, // mov  ecx, [r14+0x108]  ; size
	0x49, 0x8B, 0xBE, 0x90, 0x01, 0x00, 0x00, // mov  rdi, [r14+0x190]  ; alloc buf
	0xF3, 0xA4,                              // rep  movsb
	// Set readback header
	0x41, 0xC7, 0x86, 0x94, 0x40, 0x00, 0x00, // mov  dword [r14+0x4094], 0
	      0x00, 0x00, 0x00, 0x00,
	0x41, 0x8B, 0x86, 0x08, 0x01, 0x00, 0x00, // mov  eax, [r14+0x108]
	0x83, 0xC0, 0x03,                        // add  eax, 3
	0xC1, 0xE8, 0x02,                        // shr  eax, 2            ; dim1=ceil(n/4)
	0x41, 0x89, 0x86, 0x98, 0x40, 0x00, 0x00, // mov  [r14+0x4098], eax
	0x41, 0xC7, 0x86, 0x9C, 0x40, 0x00, 0x00, // mov  dword [r14+0x409C], 1
	      0x01, 0x00, 0x00, 0x00,
	0x49, 0x8B, 0x86, 0x90, 0x01, 0x00, 0x00, // mov  rax, [r14+0x190]
	0x49, 0x89, 0x86, 0xB0, 0x40, 0x00, 0x00, // mov  [r14+0x40B0], rax ; dataPtr
	// .fail: epilogue
	0x48, 0x83, 0xC4, 0x38,                  // add  rsp, 0x38
	0x41, 0x5E,                              // pop  r14
	0x5D,                                     // pop  rbp
	0x31, 0xC0,                              // xor  eax, eax
	0xC3                                      // ret
};

// Per-thread response buffer. Each thread writes its own rspPtr into its own
// command; the kernel writes back to that per-thread address. No shared state,
// no mutex needed â€” multiple threads can issue IOCTLs truly in parallel.
struct __declspec(align(16)) ThreadRsp { RspBuffer rsp; };
inline ThreadRsp& GetThreadRsp() {
	static thread_local ThreadRsp tl = {};
	return tl;
}
// Legacy accessor so existing code reading g_rsp.* works per-thread.
#define tl_rsp (GetThreadRsp())

inline bool SendCmd(DWORD cmdCode, const void* params, DWORD paramSize, bool suppressLog = false)
{
	ThreadRsp& rsp = GetThreadRsp();
	memset(&rsp.rsp, 0, sizeof(rsp.rsp));

	__declspec(align(16)) CmdBuffer cmd = {};
	cmd.size   = CMD_BUF_SIZE;
	cmd.magic  = CMD_MAGIC;
	cmd.nonce  = GetTickCount();
	cmd.cmd    = cmdCode;
	cmd.rspPtr = (UINT64)&rsp.rsp;  // per-thread response target
	if (params && paramSize > 0)
		memcpy(cmd.params, params, min(paramSize, (DWORD)sizeof(cmd.params)));

	DWORD written = 0;
	if (!WriteFile(g_dev, &cmd, CMD_BUF_SIZE, &written, nullptr)) {
		DRV_ERR("SendCmd(%u) WriteFile failed â€” GLE=%u", cmdCode, GetLastError());
		return false;
	}
	if ((INT32)rsp.rsp.status < 0 && !suppressLog)
		DRV_ERR("SendCmd(%u) NTSTATUS=0x%08X", cmdCode, rsp.rsp.status);
	return (INT32)rsp.rsp.status >= 0;
}

inline bool KernelRead(uint64_t srcKVA, void* dst, uint32_t size)
{
	struct { UINT64 srcKVA; UINT64 dstPtr; DWORD sz; } p;
	p.srcKVA = srcKVA; p.dstPtr = (UINT64)dst; p.sz = size;
	return SendCmd(CMD_READ_KERNEL_MEM, &p, sizeof(p));
}

namespace wq { inline void InvalidateCache(); }  // fwd-decl (defined below)

// Open a process handle via cmd 785. Driver uses ObOpenObjectByPointer(KernelMode)
// which bypasses DACL and gives us PROCESS_ALL_ACCESS. Caches the handle.
inline HANDLE GetTargetHandle(uint32_t pid)
{
	if (g_targetH && g_targetPid == pid) return g_targetH;

	if (g_targetH) {
		CloseHandle(g_targetH);
		g_targetH = nullptr;
		wq::InvalidateCache();  // PID changed â€” flush stale write-dedup cache
	}

	// params: [0x00] PID (DWORD), [0x04] AccessMask (DWORD)
	struct { DWORD pid; DWORD access; } p;
	p.pid = pid;
	p.access = 0x1FFFFF;  // PROCESS_ALL_ACCESS

	if (!SendCmd(CMD_OPEN_PROCESS, &p, sizeof(p))) {
		DRV_ERR("CMD_OPEN_PROCESS(785) failed for PID=%u status=0x%X", pid, tl_rsp.rsp.status);
		return nullptr;
	}

	HANDLE h = *(HANDLE*)(tl_rsp.rsp.data);
	if (!h || h == INVALID_HANDLE_VALUE) {
		DRV_ERR("CMD_OPEN_PROCESS(785) returned NULL handle for PID=%u", pid);
		return nullptr;
	}

	DRV_LOG("CMD_OPEN_PROCESS(785) gave handle 0x%llX for PID=%u", (unsigned long long)h, pid);
	g_targetH = h;
	g_targetPid = pid;
	return h;
}

// Cmd 787: cross-process read with handle access-mask bypass via sub_140007874 ladder.
// The ladder tries ObReferenceObjectByHandle with 18 different access masks until
// one succeeds. Even if EAC strips PROCESS_VM_READ from our handle, ANY remaining
// access bit (SYNCHRONIZE, etc.) lets the kernel resolve the PEPROCESS pointer.
// The actual read then runs in kernel context bypassing all access checks.
// Params layout: [0x00] HANDLE, [0x08] srcVA, [0x10] dstPtr, [0x18] size
inline bool ProcessRead(uint32_t pid, uint64_t srcVA, void* dst, uint32_t size)
{
	if (size == 0) return true;
	HANDLE h = GetTargetHandle(pid);
	if (!h) return false;

	struct {
		UINT64 handle;
		UINT64 srcVA;
		UINT64 dstPtr;
		DWORD  size;
	} p;
	p.handle = (UINT64)h;
	p.srcVA  = srcVA;
	p.dstPtr = (UINT64)dst;
	p.size   = size;

	SendCmd(CMD_READ_PROCESS_MEM, &p, sizeof(p), true);
	DWORD copied = *(DWORD*)(tl_rsp.rsp.data);
	return copied == size;
}

// Low-level synchronous write â€” expensive (10-50ms inject). Only called from
// the background write worker thread. Never call from render loop directly.
// Low-level synchronous write — expensive (10-50ms inject). Only called from
// the background write worker thread. Never call from render loop directly.
inline bool ProcessWriteSync(uint32_t pid, uint64_t dstVA, const void* src, uint32_t size)
{
	if (!g_injectReady) {
		SendCmd(CMD_INIT_INJECT, nullptr, 0);
		g_injectReady = true;
	}
	const DWORD DESC_OFF = 0x30;
	DWORD codeSize = DESC_OFF + 8 + 4 + size + 8;
	if (codeSize > 0x4000) return false;
	BYTE* code = (BYTE*)VirtualAlloc(nullptr, codeSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!code) return false;
	memset(code, 0, codeSize);
	memcpy(code, kWriteShellcode, sizeof(kWriteShellcode));
	BYTE* p = code + DESC_OFF;
	memcpy(p, &dstVA, 8); p += 8;
	memcpy(p, &size,  4); p += 4;
	memcpy(p, src, size);
	BYTE params[0x49] = {};
	*(UINT64*)(params + 0x08) = (UINT64)pid;
	*(UINT64*)(params + 0x30) = (UINT64)code;
	*(DWORD*) (params + 0x38) = codeSize;
	params[0x48]              = 0x02;
	bool injectOk = SendCmd(CMD_INJECT_EXEC, params, sizeof(params), true);
	DWORD injectSts = tl_rsp.rsp.status;
	// 0xE01AF119 = EAC screen-capture validation error â€” expected, writes already done.
	// Any other negative NTSTATUS = real failure, log it.
	if (!injectOk && injectSts != 0xE01AF119u) {
		DRV_ERR("ProcessWriteSync: CMD_815 NTSTATUS=0x%08X dst=0x%llX sz=%u",
			(unsigned)injectSts, (unsigned long long)dstVA, size);
	}
	VirtualFree(code, 0, MEM_RELEASE);
	return true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Batched synchronous write â€” packs multiple {addr,data} pairs into ONE
// CMD 815 injection. The shellcode already loops over descriptors:
//   [0x30] {QWORD destVA, DWORD len, BYTE data[]}... terminated by destVA=0
// This turns 3+ separate 10-50ms injections into 1.
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
struct WriteDesc { uint64_t dstVA; const void* src; uint32_t size; };

inline bool ProcessWriteBatch(uint32_t pid, const WriteDesc* descs, uint32_t count)
{
	if (!count) return true;
	if (!g_injectReady) {
		SendCmd(CMD_INIT_INJECT, nullptr, 0);
		g_injectReady = true;
	}

	// Calculate total payload size: shellcode + descriptors + terminator
	const DWORD DESC_OFF = 0x30;
	DWORD payloadSize = 0;
	for (uint32_t i = 0; i < count; i++)
		payloadSize += 8 + 4 + descs[i].size;  // destVA + len + data per descriptor
	payloadSize += 8;  // zero terminator

	DWORD codeSize = DESC_OFF + payloadSize;
	if (codeSize > 0x4000) return false;

	// Use thread-local pre-allocated buffer to avoid VirtualAlloc/Free per call
	static thread_local BYTE* s_codeBuf = nullptr;
	static thread_local DWORD s_codeBufSz = 0;
	if (!s_codeBuf || s_codeBufSz < codeSize) {
		if (s_codeBuf) VirtualFree(s_codeBuf, 0, MEM_RELEASE);
		s_codeBufSz = max(codeSize, (DWORD)0x1000);  // at least 4KB
		s_codeBuf = (BYTE*)VirtualAlloc(nullptr, s_codeBufSz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!s_codeBuf) { s_codeBufSz = 0; return false; }
	}

	memset(s_codeBuf, 0, codeSize);
	memcpy(s_codeBuf, kWriteShellcode, sizeof(kWriteShellcode));

	// Pack descriptors
	BYTE* p = s_codeBuf + DESC_OFF;
	for (uint32_t i = 0; i < count; i++) {
		memcpy(p, &descs[i].dstVA, 8); p += 8;
		memcpy(p, &descs[i].size,  4); p += 4;
		memcpy(p, descs[i].src, descs[i].size); p += descs[i].size;
	}
	// Terminator (destVA = 0) â€” already zeroed by memset

	BYTE params[0x49] = {};
	*(UINT64*)(params + 0x08) = (UINT64)pid;
	*(UINT64*)(params + 0x30) = (UINT64)s_codeBuf;
	*(DWORD*) (params + 0x38) = codeSize;
	params[0x48]              = 0x02;

	bool ok = SendCmd(CMD_INJECT_EXEC, params, sizeof(params), true);
	DWORD sts = tl_rsp.rsp.status;
	if (!ok && sts != 0xE01AF119u) {
		DRV_ERR("ProcessWriteBatch: CMD_815 NTSTATUS=0x%08X count=%u totalSz=%u",
			(unsigned)sts, count, payloadSize);
	}

	static int s_batchDbg = 0;
	if (s_batchDbg < 5) {
		DRV_LOG("WriteBatch[%d]: %u descriptors, %u bytes payload â€” %s",
			s_batchDbg, count, payloadSize, ok || sts == 0xE01AF119u ? "OK" : "FAIL");
		s_batchDbg++;
	}
	return true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Async Write Queue â€” coalesces per-address
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//   - tWrite() enqueues and returns in microseconds (render never blocks)
//   - Worker thread drains FIFO, calling ProcessWriteSync
//   - Uses condition_variable instead of Sleep(1) for instant wake-up
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
namespace wq {
	struct Entry { uint32_t pid; uint64_t addr; std::vector<uint8_t> bytes; };
	static std::mutex                                        mu;
	static std::condition_variable                           cv;
	static std::deque<Entry>                                 queue;
	static std::unordered_map<uint64_t, Entry*>              pending;
	static std::atomic<bool>                                 running{false};
	static std::thread                                       worker;
	static std::atomic<uint32_t>                             s_dispatched{0};
	static std::atomic<uint32_t>                             s_failed{0};

	inline void Worker() {
		while (running.load(std::memory_order_acquire)) {
			Entry e;
			{
				std::unique_lock<std::mutex> lk(mu);
				cv.wait_for(lk, std::chrono::milliseconds(1), [] { return !queue.empty(); });
				if (queue.empty()) continue;
				e = std::move(queue.front());
				queue.pop_front();
				pending.erase(e.addr);
			}
			try {
				ProcessWriteSync(e.pid, e.addr, e.bytes.data(), (uint32_t)e.bytes.size());
			} catch (...) {
				++s_failed;
				DRV_ERR("wq::Worker exception for addr=0x%llX sz=%u â€” continuing",
					(unsigned long long)e.addr, (unsigned)e.bytes.size());
			}
			uint32_t n = ++s_dispatched;
			if (n % 100 == 0)
				DRV_LOG("wq::Worker dispatched=%u failed=%u queueDepth=%zu",
					n, (unsigned)s_failed.load(), queue.size());
		}
	}

	inline void Start() {
		if (running.exchange(true)) return;
		worker = std::thread(Worker);
	}

	inline void InvalidateCache() {
		std::lock_guard<std::mutex> lk(mu);
		queue.clear();
		pending.clear();
	}
}

inline bool ProcessWrite(uint32_t pid, uint64_t dstVA, const void* src, uint32_t size)
{
	if (size == 0) return true;
	if (size > 256) return ProcessWriteSync(pid, dstVA, src, size);

	wq::Start();
	const uint8_t* srcBytes = (const uint8_t*)src;

	{
		std::lock_guard<std::mutex> lk(wq::mu);
		auto it = wq::pending.find(dstVA);
		if (it != wq::pending.end()) {
			it->second->bytes.assign(srcBytes, srcBytes + size);
			wq::cv.notify_one();
			return true;
		}
		wq::queue.push_back({ pid, dstVA, std::vector<uint8_t>(srcBytes, srcBytes + size) });
		wq::pending[dstVA] = &wq::queue.back();
	}
	wq::cv.notify_one();

	static int s_writeDbg = 0;
	if (s_writeDbg < 5) {
		DRV_LOG("ProcessWrite[%d]: dst=0x%llX sz=%u (queued)",
			s_writeDbg, (unsigned long long)dstVA, size);
		s_writeDbg++;
	}
	return true;
}

inline UINT64 GetNtoskrnlBase()
{
	DWORD needed = 0;
	EnumDeviceDrivers(nullptr, 0, &needed);
	std::vector<LPVOID> drv(needed / sizeof(LPVOID));
	EnumDeviceDrivers(drv.data(), needed, &needed);
	char name[MAX_PATH];
	for (auto d : drv) {
		GetDeviceDriverBaseNameA(d, name, sizeof(name));
		if (_stricmp(name, "ntoskrnl.exe") == 0 || _stricmp(name, "ntkrnlmp.exe") == 0)
			return (UINT64)d;
	}
	return 0;
}

struct ProcessInfo {
	uint32_t pid;
	uint64_t eprocess;
	uint64_t cr3;
	uint64_t peb;
	uint64_t sectionBase;
};

inline bool FindProcess(const char* imageName, ProcessInfo& out)
{
	out = {};
	if (!g_sysEproc || !g_off.valid) return false;
	UINT64 curVA = g_sysEproc;
	for (int i = 0; i < 1024; i++) {
		char imgName[16] = {};
		DWORD pid = 0;
		KernelRead(curVA + g_off.ImageFileName, imgName, sizeof(imgName));
		KernelRead(curVA + g_off.UniqueProcessId, &pid, sizeof(pid));
		if (_stricmp(imgName, imageName) == 0) {
			// Skip zombie/terminated processes â€” ExitStatus must be STATUS_PENDING (0x103)
			LONG exitStatus = 0;
			KernelRead(curVA + g_off.ExitStatus, &exitStatus, sizeof(exitStatus));
			if (exitStatus != 0x103) {
				DRV_LOG("  skipping PID=%u EPROCESS=0x%llX (ExitStatus=0x%X, not running)",
					pid, (unsigned long long)curVA, (unsigned)exitStatus);
				// continue walking â€” the real process may be later in the list
			} else {
				out.pid      = pid;
				out.eprocess = curVA;
				KernelRead(curVA + g_off.DirectoryTableBase, &out.cr3, sizeof(out.cr3));
				KernelRead(curVA + g_off.Peb, &out.peb, sizeof(out.peb));
				KernelRead(curVA + g_off.SectionBaseAddress, &out.sectionBase, sizeof(out.sectionBase));
				return true;
			}
		}
		UINT64 flink = 0;
		if (!KernelRead(curVA + g_off.ActiveProcessLinks, &flink, sizeof(flink)) || !flink) break;
		if (flink == g_sysEproc + g_off.ActiveProcessLinks) break;
		UINT64 nextVA = flink - g_off.ActiveProcessLinks;
		if (nextVA == curVA) break;
		curVA = nextVA;
	}
	return false;
}

// â”€â”€ MMVAD offsets for Windows 10 1903+ / Windows 11 (all 22H2/23H2/24H2) â”€â”€
// These are structural offsets inside the kernel MMVAD / SUBSECTION /
// CONTROL_AREA / FILE_OBJECT structures, used to walk the VAD tree.
namespace vad {
	constexpr uint32_t Left            = 0x00;  // MMVAD.VadNode.Left
	constexpr uint32_t Right           = 0x08;  // MMVAD.VadNode.Right
	constexpr uint32_t StartingVpn     = 0x18;  // MMVAD_SHORT.StartingVpn  (ULONG)
	constexpr uint32_t EndingVpn       = 0x1C;  // MMVAD_SHORT.EndingVpn    (ULONG)
	constexpr uint32_t StartingVpnHigh = 0x20;  // MMVAD_SHORT.StartingVpnHigh (UCHAR)
	constexpr uint32_t EndingVpnHigh   = 0x21;  // MMVAD_SHORT.EndingVpnHigh   (UCHAR)
	constexpr uint32_t VadFlags        = 0x30;  // MMVAD_SHORT.u.VadFlags (ULONG)
	constexpr uint32_t Subsection      = 0x48;  // MMVAD.Subsection
	// SUBSECTION
	constexpr uint32_t ControlArea     = 0x00;  // SUBSECTION.ControlArea
	// CONTROL_AREA
	constexpr uint32_t FilePointer     = 0x40;  // CONTROL_AREA.FilePointer (EX_FAST_REF)
	// FILE_OBJECT
	constexpr uint32_t FileNameLen     = 0x58;  // FILE_OBJECT.FileName.Length
	constexpr uint32_t FileNameBuf     = 0x60;  // FILE_OBJECT.FileName.Buffer
}

inline bool IsKernelPtr(uint64_t p) { return p >= 0xFFFF000000000000ULL; }

inline uint64_t FindModuleBase(uint64_t eprocess, uint32_t pid, const wchar_t* name)
{
	if (!eprocess || !pid) return 0;

	// â”€â”€ Method 1: CreateToolhelp32Snapshot (TH32CS_SNAPMODULE) â”€â”€
	DRV_LOG("FindModuleBase: trying Toolhelp snapshot for '%ls' (pid=%u)", name, pid);
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
		if (snap != INVALID_HANDLE_VALUE) {
			MODULEENTRY32W me = {};
			me.dwSize = sizeof(me);
			if (Module32FirstW(snap, &me)) {
				do {
					if (wcsstr(me.szModule, name) || wcsstr(me.szExePath, name)) {
						uint64_t base = (uint64_t)me.modBaseAddr;
						DRV_LOG("FindModuleBase: FOUND '%ls' via Toolhelp base=0x%llX",
							name, (unsigned long long)base);
						CloseHandle(snap);
						return base;
					}
				} while (Module32NextW(snap, &me));
			}
			CloseHandle(snap);
			DRV_LOG("FindModuleBase: Toolhelp snapshot OK but '%ls' not in list", name);
		} else {
			DRV_LOG("FindModuleBase: Toolhelp failed GLE=%u, trying VAD walk", GetLastError());
		}
	}

	// â”€â”€ Method 2: Kernel VAD tree walk via CMD 788 â”€â”€
	DRV_LOG("FindModuleBase: EPROCESS=0x%llX VadRoot@+0x%X for '%ls'",
		(unsigned long long)eprocess, g_off.VadRoot, name);

	// Diagnostic: dump 8 QWORDs around the expected VadRoot offset
	{
		uint32_t base = (g_off.VadRoot >= 0x20) ? g_off.VadRoot - 0x20 : 0;
		DRV_LOG("FindModuleBase: dumping EPROCESS+0x%X..+0x%X:", base, base + 0x40);
		for (uint32_t off = base; off < base + 0x40; off += 8) {
			uint64_t val = 0;
			KernelRead(eprocess + off, &val, 8);
			DRV_LOG("  +0x%03X = 0x%llX%s", off, (unsigned long long)val,
				(off == g_off.VadRoot) ? "  <-- VadRoot" : "");
		}
	}

	uint64_t vadRoot = 0;
	KernelRead(eprocess + g_off.VadRoot, &vadRoot, 8);
	if (!vadRoot || !IsKernelPtr(vadRoot)) {
		DRV_ERR("FindModuleBase: VadRoot=0x%llX (invalid)", (unsigned long long)vadRoot);
		return 0;
	}
	DRV_LOG("FindModuleBase: VadRoot=0x%llX", (unsigned long long)vadRoot);

	std::vector<uint64_t> stack;
	stack.reserve(256);
	stack.push_back(vadRoot);

	int visited = 0, images = 0;
	uint64_t result = 0;

	// Collect every image filename + base seen during the walk so we can dump
	// them when the requested module isn't found. Lets us see what Rust has
	// renamed/protected GameAssembly.dll into.
	struct ImageEntry { uint64_t base; wchar_t file[MAX_PATH]; };
	std::vector<ImageEntry> all_images;
	all_images.reserve(256);

	while (!stack.empty() && visited < 4096) {
		uint64_t node = stack.back();
		stack.pop_back();
		if (!node || !IsKernelPtr(node)) continue;
		visited++;

		uint64_t left = 0, right = 0;
		KernelRead(node + vad::Left,  &left,  8);
		KernelRead(node + vad::Right, &right, 8);
		if (right && IsKernelPtr(right)) stack.push_back(right);
		if (left  && IsKernelPtr(left))  stack.push_back(left);

		uint32_t vadFlags = 0;
		KernelRead(node + vad::VadFlags, &vadFlags, 4);
		uint32_t vadType = (vadFlags >> 4) & 7;
		if (vadType != 2) continue;

		images++;

		uint32_t startVpn = 0;
		uint8_t  startHi  = 0;
		KernelRead(node + vad::StartingVpn,     &startVpn, 4);
		KernelRead(node + vad::StartingVpnHigh, &startHi,  1);
		uint64_t moduleBase = ((uint64_t)startHi << 32 | startVpn) << 12;

		uint64_t subsection = 0;
		KernelRead(node + vad::Subsection, &subsection, 8);
		if (!subsection || !IsKernelPtr(subsection)) continue;

		uint64_t controlArea = 0;
		KernelRead(subsection + vad::ControlArea, &controlArea, 8);
		if (!controlArea || !IsKernelPtr(controlArea)) continue;

		uint64_t fileRef = 0;
		KernelRead(controlArea + vad::FilePointer, &fileRef, 8);
		uint64_t fileObj = fileRef & ~0xFULL;
		if (!fileObj || !IsKernelPtr(fileObj)) continue;

		uint16_t nameLen = 0;
		uint64_t nameBuf = 0;
		KernelRead(fileObj + vad::FileNameLen, &nameLen, 2);
		KernelRead(fileObj + vad::FileNameBuf, &nameBuf, 8);
		if (!nameLen || !nameBuf || !IsKernelPtr(nameBuf)) continue;

		wchar_t filename[MAX_PATH] = {};
		uint32_t readLen = min((uint32_t)nameLen, (uint32_t)((MAX_PATH - 1) * 2));
		KernelRead(nameBuf, filename, readLen);

		// Record this image â€” even if it doesn't match â€” so we can dump it later.
		if (all_images.size() < all_images.capacity()) {
			ImageEntry e{};
			e.base = moduleBase;
			wcsncpy_s(e.file, filename, _TRUNCATE);
			all_images.push_back(e);
		}

		if (wcsstr(filename, name)) {
			result = moduleBase;
			DRV_LOG("FindModuleBase: FOUND '%ls' base=0x%llX (file: %ls)",
				name, (unsigned long long)result, filename);
			break;
		}
	}

	DRV_LOG("FindModuleBase: visited %d VAD nodes, %d image maps", visited, images);
	if (!result) {
		DRV_ERR("FindModuleBase: '%ls' not found in VAD tree", name);

		// â”€â”€ Dump ALL image filenames found in the VAD walk â”€â”€
		// Helps identify when the module has been renamed or repacked.
		// We focus on:
		//   1. Files in the Rust install folder (likely the IL2CPP carrier)
		//   2. Files with "Assembly" / "il2cpp" / "Game" / "Mono" / "client" in the name
		//   3. Anonymous mappings with .dll-sized regions
		DRV_LOG("FindModuleBase: dumping all %zu image-mapped files seen:", all_images.size());
		int dumped = 0;
		for (auto& img : all_images) {
			// Only dump entries whose path looks game-relevant or unknown
			bool game_relevant =
				wcsstr(img.file, L"Rust") || wcsstr(img.file, L"Bundle") ||
				wcsstr(img.file, L"Assembly") || wcsstr(img.file, L"il2cpp") ||
				wcsstr(img.file, L"Il2Cpp") || wcsstr(img.file, L"GameAssembly") ||
				wcsstr(img.file, L"client") || wcsstr(img.file, L"Mono") ||
				wcsstr(img.file, L"Player.dll") || wcsstr(img.file, L"Native");
			// Always include the first ~30 entries for full visibility
			if (game_relevant || dumped < 30) {
				DRV_LOG("  [%2d] base=0x%012llX  file=%ls", dumped,
					(unsigned long long)img.base, img.file);
				dumped++;
			}
			if (dumped >= 60) break;
		}
		DRV_LOG("FindModuleBase: end of image-map dump");
	}
	return result;
}

} // namespace xhdr

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Driver adapter class  (identical public API to the old WinNotify Driver)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

class Driver
{
private:
	uint64_t CurrentCr3;
	uint64_t CurrentPeb;
	uint64_t CurrentBase;

public:
	uint32_t ProcessId;

	Driver() : CurrentCr3(0), CurrentPeb(0), CurrentBase(0), ProcessId(0) {}
	~Driver() {}

	__forceinline void Setup(HANDLE) {}  // handle is managed by xhdr globals

	__forceinline void SetFromProcessInfo(const xhdr::ProcessInfo& info)
	{
		ProcessId   = info.pid;
		CurrentCr3  = info.cr3 ? info.cr3 : 1;  // non-zero so callers don't bail
		CurrentPeb  = info.peb;
		CurrentBase = info.sectionBase;
	}

	__forceinline uint32_t Target(const wchar_t* ProcessName)
	{
		PROCESSENTRY32 Entry;
		Entry.dwSize = sizeof(PROCESSENTRY32);
		HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (Snapshot == INVALID_HANDLE_VALUE) return 0;
		if (Process32First(Snapshot, &Entry)) {
			do {
				if (wcscmp(Entry.szExeFile, ProcessName) == 0) {
					ProcessId = Entry.th32ProcessID;
					CloseHandle(Snapshot);
					return ProcessId;
				}
			} while (Process32Next(Snapshot, &Entry));
		}
		CloseHandle(Snapshot);
		return 0;
	}

	__forceinline uint32_t GetProcessId() { return ProcessId; }

	__forceinline void UpdateCr3()
	{
		// CR3 is populated by SetFromProcessInfo via EPROCESS walk.
		// Ensure non-zero so any callers don't treat it as failure.
		if (!CurrentCr3) CurrentCr3 = 1;
	}

	__forceinline uint64_t GetCr3()    { return CurrentCr3;  }
	__forceinline uint64_t GetPeb()    { return CurrentPeb;  }
	__forceinline uint64_t GetProcessBase() { return CurrentBase; }

	__forceinline bool Read(uint64_t Address, void* Buffer, size_t Size)
	{
		if (!ProcessId) return false;
		return xhdr::ProcessRead(ProcessId, Address, Buffer, (uint32_t)Size);
	}

	__forceinline bool Write(uint64_t Address, void* Buffer, size_t Size)
	{
		if (!ProcessId) return false;
		return xhdr::ProcessWrite(ProcessId, Address, Buffer, (uint32_t)Size);
	}

	template<typename T>
	__forceinline T Read(uint64_t Address)
	{
		T Value{};
		Read(Address, &Value, sizeof(T));
		return Value;
	}

	template<typename T>
	__forceinline bool Write(uint64_t Address, T Value)
	{
		return Write(Address, &Value, sizeof(T));
	}
};

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// drv namespace  (identical public API to the old WinNotify version)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

namespace drv {
	inline Driver*   driver   = nullptr;
	inline uint32_t  procid   = 0;
	inline uint64_t  Base     = 0;
	inline uint64_t  eprocess = 0;   // kernel VA of target EPROCESS

	inline bool Init(bool dummy = false)
	{
		// Parse EPROCESS offsets dynamically from ntoskrnl.exe on disk
		DRV_LOG("Resolving EPROCESS offsets from ntoskrnl.exe on disk...");
		if (!NtoskrnlParser::Resolve(xhdr::g_off)) {
			DRV_ERR("NtoskrnlParser::Resolve failed");
			return false;
		}
		XDRV_OK("EPROCESS offsets resolved (ImageFileName=0x%X UniqueProcessId=0x%X Peb=0x%X SectionBase=0x%X)",
			(unsigned)xhdr::g_off.ImageFileName, (unsigned)xhdr::g_off.UniqueProcessId,
			(unsigned)xhdr::g_off.Peb, (unsigned)xhdr::g_off.SectionBaseAddress);

		// Open xhunter1 device
		DRV_LOG("Opening \\\\.\\xhunter1 device...");
		xhdr::g_dev = CreateFileW(L"\\\\.\\xHunters",
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (xhdr::g_dev == INVALID_HANDLE_VALUE) {
			DRV_ERR("CreateFileW failed â€” GLE=%u (driver not loaded?)", GetLastError());
			return false;
		}
		XDRV_OK("Device handle opened");

		// cmd 777: register our process
		DRV_LOG("Sending CMD_REGISTER (777)...");
		if (!xhdr::SendCmd(xhdr::CMD_REGISTER, nullptr, 0)) {
			DRV_ERR("CMD_REGISTER failed");
			CloseHandle(xhdr::g_dev);
			xhdr::g_dev = INVALID_HANDLE_VALUE;
			return false;
		}
		XDRV_OK("CMD_REGISTER OK");

		// cmd 775: set flags
		DRV_LOG("Sending CMD_SET_FLAGS (775)...");
		struct { DWORD pid; DWORD flags; } regp;
		regp.pid = GetCurrentProcessId(); regp.flags = 0x0008;
		if (xhdr::SendCmd(xhdr::CMD_SET_FLAGS, &regp, sizeof(regp)))
			XDRV_OK("CMD_SET_FLAGS OK");
		else
			DRV_ERR("CMD_SET_FLAGS returned non-zero status (non-fatal, continuing)");

		// cmd 801: init injection subsystem (resolves RtlCreateUserThread etc.)
		DRV_LOG("Sending CMD_INIT_INJECT (801)...");
		xhdr::SendCmd(xhdr::CMD_INIT_INJECT, nullptr, 0);
		xhdr::g_injectReady = true;
		XDRV_OK("CMD_INIT_INJECT sent");

		// Resolve System EPROCESS
		DRV_LOG("Locating ntoskrnl.exe base...");
		xhdr::g_ntBase = xhdr::GetNtoskrnlBase();
		if (!xhdr::g_ntBase) {
			DRV_ERR("GetNtoskrnlBase returned 0");
			return false;
		}
		XDRV_OK("ntoskrnl.exe base = 0x%llX", (unsigned long long)xhdr::g_ntBase);

		DRV_LOG("Reading PsInitialSystemProcess pointer...");
		UINT64 psiVA = xhdr::g_ntBase + xhdr::g_off.PsInitialSystemProcessRVA;
		if (!xhdr::KernelRead(psiVA, &xhdr::g_sysEproc, sizeof(xhdr::g_sysEproc)) || !xhdr::g_sysEproc) {
			DRV_ERR("KernelRead PsInitialSystemProcess failed (psiVA=0x%llX)", (unsigned long long)psiVA);
			return false;
		}
		XDRV_OK("System EPROCESS = 0x%llX", (unsigned long long)xhdr::g_sysEproc);

		driver = new Driver();
		XDRV_OK("Driver object created â€” Init complete");
		return true;
	}

	inline void ReadPhys(PVOID address, PVOID buffer, DWORD size)
	{
		if (!driver || !procid) return;
		xhdr::ProcessRead(procid, (uint64_t)address, buffer, size);
	}

	inline void WritePhys(PVOID address, PVOID buffer, DWORD size)
	{
		if (!driver || !procid) return;
		xhdr::ProcessWrite(procid, (uint64_t)address, buffer, size);
	}

	inline uint64_t GetBase()
	{
		if (!driver || !procid) return 0;
		return driver->GetProcessBase();
	}

	inline uint32_t FindProcess(const wchar_t* process_name)
	{
		if (!driver) return 0;
		// Convert to narrow char for EPROCESS ImageFileName comparison
		char narrowName[64] = {};
		WideCharToMultiByte(CP_UTF8, 0, process_name, -1, narrowName, sizeof(narrowName), nullptr, nullptr);
		DRV_LOG("Walking EPROCESS list for '%s'...", narrowName);
		xhdr::ProcessInfo info;
		if (xhdr::FindProcess(narrowName, info)) {
			procid   = info.pid;
			eprocess = info.eprocess;
			driver->SetFromProcessInfo(info);
			XDRV_OK("Found '%s' â€” PID=%u EPROCESS=0x%llX CR3=0x%llX PEB=0x%llX",
				narrowName, info.pid,
				(unsigned long long)info.eprocess,
				(unsigned long long)info.cr3,
				(unsigned long long)info.peb);
			return procid;
		}
		DRV_ERR("'%s' not found in EPROCESS list", narrowName);
		return 0;
	}

	inline uint64_t get_module(const wchar_t* name)
	{
		if (!driver || !procid || !eprocess) return 0;
		uint64_t base = xhdr::FindModuleBase(eprocess, procid, name);
		char narrow[128] = {};
		WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
		if (base)
			XDRV_OK("%-28s base = 0x%llX", narrow, (unsigned long long)base);
		else
			DRV_ERR("%-28s NOT FOUND in PEB LDR", narrow);
		return base;
	}
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Global memory helpers  (identical API â€” used throughout the codebase)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

inline bool Valid(const uint64_t adress)
{
	if (adress <= 0x400000 || adress == 0xCCCCCCCCCCCCCCCC ||
		reinterpret_cast<void*>(adress) == nullptr || adress > 0x7FFFFFFFFFFFFFFF)
		return false;
	return true;
}

template <typename T>
inline T read(uint64_t address)
{
	T buffer{};
	drv::ReadPhys((PVOID)address, &buffer, sizeof(T));
	return buffer;
}

template <typename T>
inline T write(uint64_t address, T buffer)
{
	drv::WritePhys((PVOID)address, &buffer, sizeof(T));
	return buffer;
}

struct UnityStrBuff {
	char buffer[128];
};

template <typename T>
inline T tRead(uint64_t address) {
	T buffer{};
	drv::ReadPhys((PVOID)address, &buffer, sizeof(T));
	return buffer;
}

template<typename T>
T tWrite(uint64_t address, T buffer) {
	drv::WritePhys((PVOID)address, &buffer, sizeof(T));
	return buffer;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// xhdr::healthReader â€” REMOVED (was: shellcode injection for batched health
// reads via CMD_INJECT_EXEC). Replaced by direct combat-block reads + the
// universal Damage::Tracker (`features/damage_tracker.h`) which doesn't need
// in-process code execution and has zero detection footprint.
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// tWriteBatch â€” write multiple values in ONE kernel injection call
// Usage:
//   xhdr::WriteDesc descs[] = {
//     { addr1, &val1, sizeof(val1) },
//     { addr2, &val2, sizeof(val2) },
//   };
//   tWriteBatch(descs, 2);
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
inline bool tWriteBatch(const xhdr::WriteDesc* descs, uint32_t count) {
	if (!drv::driver || !drv::procid) return false;
	return xhdr::ProcessWriteBatch(drv::procid, descs, count);
}

// Synchronous single-write (bypasses async queue, blocks until done)
template<typename T>
inline bool tWriteSync(uint64_t address, T value) {
	if (!drv::driver || !drv::procid) return false;
	return xhdr::ProcessWriteSync(drv::procid, address, &value, sizeof(T));
}

// Bulk read â€” read a contiguous block in one IOCTL instead of many small ones
inline bool tReadBulk(uint64_t address, void* dst, uint32_t size) {
	if (!drv::driver || !drv::procid) return false;
	return xhdr::ProcessRead(drv::procid, address, dst, size);
}

template<typename Type>
inline Type ReadChain(const std::uint64_t& Address, std::vector<std::uint64_t> Offsets)
{
	std::uint64_t Value = Address;
	for (int i = 0; i < (int)Offsets.size() - 1; i++)
		Value = tRead<std::uint64_t>(Value + Offsets[i]);
	return tRead<Type>(Value + Offsets.back());
}

template <typename T>
inline T readchain(uint64_t address, std::vector<uint64_t> chain)
{
	uint64_t cur_read = address;
	for (size_t i = 0; i < chain.size() - 1; ++i) {
		cur_read = read<uint64_t>(cur_read + chain[i]);
		if (!cur_read) return T();
	}
	return read<T>(cur_read + chain.back());
}

inline std::string readstring(uint64_t Address)
{
	std::unique_ptr<char[]> buffer(new char[64]);
	drv::ReadPhys((PVOID)Address, buffer.get(), 64);
	return buffer.get();
}

inline std::string read_wstr(uintptr_t address)
{
	wchar_t buffer[1024];
	drv::ReadPhys(reinterpret_cast<PVOID>(address), buffer, sizeof(buffer));
	buffer[1023] = L'\0';
	std::wstring wstr(buffer);
	for (auto& ch : wstr)
		if (ch < 32 || ch > 126) ch = L'?';
	std::string res;
	for (auto ch : wstr) res += static_cast<char>(ch);
	return res;
}

inline std::string ReadChar(uint64_t addr) {
	std::string str = read<UnityStrBuff>(addr).buffer;
	if (!str.empty()) return str;
	return "31";
}
