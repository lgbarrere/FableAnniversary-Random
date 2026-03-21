// =============================================================================
// Project      : FableAnniversary-Random
// File         : function_dumper.cpp
// Author       : FableModLoader
// Created      : 2026-03-15
// Description  : Dumps all function prototypes found in Fable.exe.
//
// Two name sources (no PDB required):
//   [PROLOG]  x86 function-prolog scan (push ebp; mov ebp, esp).
//             Identifies all non-inlined MSVC functions.
//             Calling convention and parameter count inferred from epilogue.
//   [RTTI]    MSVC Run-Time Type Information scan.
//             Finds .?AV/.?AU type-descriptor strings, resolves Complete
//             Object Locators and vtables, then labels each virtual slot as
//             ClassName::vfunc_N.
//
// Hard limits without a PDB:
//   Non-virtual function names     -> IMPOSSIBLE (stripped from binary)
//   Real parameter / return types  -> IMPOSSIBLE (no type info in .exe)
//
// Output : FableFunctions.log in the game working directory.
// =============================================================================

#include "pch.h"
#include "function_dumper.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

// UnDecorateSymbolName is the only DbgHelp function we keep (for RTTI name
// demangling).  Include DbgHelp with UNICODE suppressed so the TCHAR macros
// resolve to the narrow-string variants.
#ifdef UNICODE
#  define _FML_RESTORE_UNICODE
#  undef UNICODE
#endif
#ifdef _UNICODE
#  define _FML_RESTORE__UNICODE
#  undef _UNICODE
#endif
#include <dbghelp.h>
#ifdef _FML_RESTORE_UNICODE
#  define UNICODE
#  undef _FML_RESTORE_UNICODE
#endif
#ifdef _FML_RESTORE__UNICODE
#  define _UNICODE
#  undef _FML_RESTORE__UNICODE
#endif
#pragma comment(lib, "dbghelp.lib")

// ---------------------------------------------------------------------------
namespace {
// ---------------------------------------------------------------------------

struct FuncEntry {
    DWORD       rva;
    ULONG_PTR   address;
    std::string name; // from RTTI; empty = unnamed
};

// Static analysis result for one function body.
struct FuncAnalysis {
    const char* callConv  = "__cdecl"; // "__cdecl" | "__stdcall" | "__thiscall"
    int         paramCount = -1;       // -1 = unknown; >= 0 = stack param count
    bool        isThiscall = false;    // true -> emit implicit "void* this"
};

FILE* g_fp = nullptr;

void WriteLine(const char* fmt, ...) {
    if (!g_fp) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_fp, fmt, a);
    va_end(a);
    fputc('\n', g_fp);
}

// ---------------------------------------------------------------------------
// Demangle an MSVC RTTI mangled class name (e.g. ".?AVCThing@@" -> "CThing").
// ---------------------------------------------------------------------------
static std::string DemangleRTTIName(const char* mangled) {
    if (mangled[0] == '.') {
        // Prepend "??" to make a name UnDecorateSymbolName can process.
        std::string attempt = "??";
        attempt += (mangled + 1); // skip the leading '.'
        char buf[512];
        if (UnDecorateSymbolName(attempt.c_str(), buf, sizeof(buf),
                                 UNDNAME_NAME_ONLY) != 0) {
            std::string result(buf);
            auto pos = result.find(" `RTTI"); // strip trailing RTTI suffix
            if (pos != std::string::npos) result.erase(pos);
            if (!result.empty()) return result;
        }
        // Fallback: extract between ".?AV"/".?AU" and "@@".
        const char* start = mangled + 4;
        const char* end   = strstr(start, "@@");
        if (end && end > start) return std::string(start, end);
    }
    return std::string(mangled);
}

// ---------------------------------------------------------------------------
// RTTI scanner: find MSVC type descriptors, resolve vtables, label vfuncs.
//
// MSVC x86 RTTI layout:
//   TypeDescriptor (in .data):
//     +00  DWORD  pVFTable  VA of type_info's internal vtable
//     +04  DWORD  spare     NULL
//     +08  char   name[]    mangled name, e.g. ".?AVCThing@@"
//
//   CompleteObjectLocator (COL, in .rdata):
//     +00  DWORD  signature  0 for x86
//     +04  DWORD  offset     vtable offset within the class
//     +08  DWORD  cdOffset
//     +0C  DWORD  pTypeDes   VA of TypeDescriptor
//     +10  DWORD  pHierDes   VA of RTTIClassHierarchyDescriptor
//
//   Vtable (in .rdata):
//     [-4]  DWORD  pCOL     VA of the CompleteObjectLocator
//     [ 0]  DWORD  vfunc_0  VA of virtual function 0
//     [ 4]  DWORD  vfunc_1
//     ...
// ---------------------------------------------------------------------------
static void CollectRTTINames(BYTE* base, DWORD imageSize,
                              std::unordered_map<DWORD, std::string>& names) {
    if (imageSize < 0x1000) return;

    auto* dh = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dh->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nh = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dh->e_lfanew);
    if (nh->Signature != IMAGE_NT_SIGNATURE) return;

    const ULONG_PTR moduleBase = reinterpret_cast<ULONG_PTR>(base);

    auto inImage = [&](ULONG_PTR va) -> bool {
        return va >= moduleBase && va < moduleBase + imageSize;
    };
    auto vaToPtr = [&](ULONG_PTR va) -> BYTE* {
        return base + (va - moduleBase);
    };

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nh);
    const WORD numSec = nh->FileHeader.NumberOfSections;
    auto rvaIsCode = [&](DWORD rva) -> bool {
        for (WORD s = 0; s < numSec; ++s)
            if (rva >= sec[s].VirtualAddress &&
                rva <  sec[s].VirtualAddress + sec[s].Misc.VirtualSize)
                return (sec[s].Characteristics &
                        (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)) != 0;
        return false;
    };

    // --- Step 1: find TypeDescriptors by scanning for ".?AV" / ".?AU" ---
    struct TypeDesc { ULONG_PTR va; std::string className; };
    std::vector<TypeDesc> typeDescs;

    for (DWORD off = 8; off + 12 < imageSize; ++off) {
        BYTE* p = base + off;
        if (p[0] != '.' || p[1] != '?' || p[2] != 'A') continue;
        if (p[3] != 'V' && p[3] != 'U') continue;

        size_t len = 0;
        while (off + len < imageSize && p[len] != '\0') ++len;
        if (len < 6 || len > 256) continue;

        // TypeDescriptor base is 8 bytes before the name string.
        const ULONG_PTR tdVA = moduleBase + off - 8;
        if (!inImage(tdVA)) continue;

        // Validate: spare (offset +4) == NULL, pVFTable (offset +0) in image.
        if (*reinterpret_cast<DWORD*>(vaToPtr(tdVA + 4)) != 0) continue;
        const DWORD pvf = *reinterpret_cast<DWORD*>(vaToPtr(tdVA));
        if (!inImage(pvf)) continue;

        std::string className = DemangleRTTIName(reinterpret_cast<const char*>(p));
        if (!className.empty())
            typeDescs.push_back({ tdVA, className });
    }

    // --- Steps 2-5: COL lookup -> vtable walk ---
    for (auto& td : typeDescs) {
        const DWORD tdVA32 = static_cast<DWORD>(td.va);

        // Find all DWORDs equal to tdVA32 (COL.pTypeDes field at COL+0x0C).
        for (DWORD off = 0x10; off + 4 <= imageSize; off += 4) {
            if (*reinterpret_cast<DWORD*>(base + off) != tdVA32) continue;

            // COL base = this field - 0x0C.
            const ULONG_PTR colVA = moduleBase + off - 0x0C;
            BYTE* col = vaToPtr(colVA);

            // Validate COL.signature == 0 and vtable offset is sane.
            if (*reinterpret_cast<DWORD*>(col)     != 0) continue;
            const DWORD vtblOffset = *reinterpret_cast<DWORD*>(col + 4);
            if (vtblOffset > 0x1000) continue;

            // Validate pHierDes (COL+0x10) points inside the image.
            if (!inImage(*reinterpret_cast<DWORD*>(col + 0x10))) continue;

            // Find vtable: vtable[-4] == VA(COL).
            const DWORD colVA32 = static_cast<DWORD>(colVA);
            for (DWORD off2 = 4; off2 + 4 <= imageSize; off2 += 4) {
                if (*reinterpret_cast<DWORD*>(base + off2) != colVA32) continue;

                // Walk the vtable entries starting at off2+4.
                for (int vIdx = 0; vIdx < 256; ++vIdx) {
                    const DWORD voff  = off2 + 4 + vIdx * 4;
                    if (voff + 4 > imageSize) break;

                    const DWORD funcVA = *reinterpret_cast<DWORD*>(base + voff);
                    if (!inImage(funcVA)) break;

                    const DWORD funcRVA = funcVA - static_cast<DWORD>(moduleBase);
                    if (!rvaIsCode(funcRVA)) break;

                    // ClassName::vfunc_N  (or  ClassName::vfunc_N [base+M])
                    std::string label = td.className + "::vfunc_" + std::to_string(vIdx);
                    if (vtblOffset > 0)
                        label += " [base+" + std::to_string(vtblOffset) + "]";

                    if (names.find(funcRVA) == names.end())
                        names[funcRVA] = label;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// x86 function body analysis
//
// Calling convention:
//   [RTTI / thiscall]  89 4D ??  (mov [ebp-N], ecx) in first 48 bytes
//   [stdcall]          5D C2 NN NN  (pop ebp; ret N) in epilogue
//   [cdecl]            5D C3        (pop ebp; ret)   in epilogue
//
// Parameter count:
//   stdcall/thiscall : N from "ret N"  (exact)
//   cdecl            : highest positive [ebp+N] access in the body (estimate)
// ---------------------------------------------------------------------------
static FuncAnalysis AnalyzeFunction(const BYTE* start, size_t maxBytes) {
    FuncAnalysis r;
    if (maxBytes < 4) return r;
    const size_t sz = (maxBytes < 8192) ? maxBytes : 8192;

    // --- Thiscall detection ---
    for (size_t i = 3; i + 2 < sz && i < 48; ++i) {
        if (start[i] == 0x89 && start[i+1] == 0x4D && start[i+2] >= 0x80) {
            r.isThiscall = true;
            r.callConv   = "__thiscall";
            break;
        }
        if (start[i] == 0xCC) break; // hit int3 padding
    }

    // --- Epilogue scan (calling convention + ret N) ---
    int bestRetN = -1;
    for (size_t i = 1; i + 1 < sz; ++i) {
        const BYTE prev = start[i-1];
        const BYTE cur  = start[i];
        const bool epilog = (prev == 0x5D /*pop ebp*/ || prev == 0xC9 /*leave*/);

        if (epilog && cur == 0xC3) {
            if (bestRetN < 0) bestRetN = 0;
        }
        if (epilog && cur == 0xC2 && i + 2 < sz) {
            const WORD n = *reinterpret_cast<const WORD*>(start + i + 1);
            if (n <= 256 && (n % 4) == 0 && (int)n > bestRetN) {
                bestRetN = (int)n;
                if (!r.isThiscall) r.callConv = "__stdcall";
            }
        }
    }

    if (bestRetN > 0) { r.paramCount = bestRetN / 4; return r; }

    // --- [ebp+N] access scan for cdecl / thiscall param count ---
    static const BYTE kEbpModRM[] = { 0x45,0x4D,0x55,0x5D,0x65,0x6D,0x75,0x7D };
    int maxDisp = 0;
    for (size_t i = 0; i + 2 < sz; ++i) {
        const BYTE modrm = start[i+1];
        bool isEbp = false;
        for (BYTE m : kEbpModRM) { if (modrm == m) { isEbp = true; break; } }
        if (!isEbp) continue;
        const BYTE disp = start[i+2];
        if (disp >= 0x08 && disp <= 0x4C && (disp % 4) == 0 && (int)disp > maxDisp)
            maxDisp = (int)disp;
    }
    if (maxDisp >= 8) {
        const int count = (maxDisp - 4) / 4;
        if (count >= 1 && count <= 12) r.paramCount = count;
    }

    return r;
}

// ---------------------------------------------------------------------------
// Build prototype string:
//   int __callconv name(void* this, int param_1, ...)
// ---------------------------------------------------------------------------
static std::string BuildPrototype(DWORD rva, const std::string& name,
                                  const FuncAnalysis& a) {
    // If the RTTI name already contains '(' it is already a full prototype.
    if (!name.empty() && name.find('(') != std::string::npos) return name;

    std::string fn;
    if (!name.empty()) {
        fn = name;
    } else {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "sub_%X", (unsigned)rva);
        fn = tmp;
    }

    std::string proto = "int ";
    proto += a.callConv;
    proto += " ";
    proto += fn;
    proto += "(";

    const int sp = (a.paramCount > 0) ? a.paramCount : 0;

    if (a.isThiscall) {
        proto += "void* this";
        for (int i = 0; i < sp; ++i)
            proto += ", int param_" + std::to_string(i + 1);
    } else if (a.paramCount < 0) {
        proto += "...";
    } else if (sp == 0) {
        proto += "void";
    } else {
        for (int i = 0; i < sp; ++i) {
            if (i) proto += ", ";
            proto += "int param_" + std::to_string(i + 1);
        }
    }

    proto += ")";
    return proto;
}

// ---------------------------------------------------------------------------
// Prolog scanner: walk executable sections, emit an entry per 55 8B EC hit.
// ---------------------------------------------------------------------------
static void ScanCodeSections(BYTE* base, DWORD imageSize,
                             std::vector<FuncEntry>& out) {
    auto* dh = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dh->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nh = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dh->e_lfanew);
    if (nh->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nh);
    const WORD numSec = nh->FileHeader.NumberOfSections;

    for (WORD s = 0; s < numSec; ++s) {
        IMAGE_SECTION_HEADER& sec = sh[s];
        if (!(sec.Characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)))
            continue;
        const DWORD secRva  = sec.VirtualAddress;
        DWORD secSize = sec.Misc.VirtualSize ? sec.Misc.VirtualSize : sec.SizeOfRawData;
        if (secRva + secSize > imageSize) secSize = imageSize - secRva;
        if (secSize < 4) continue;

        BYTE* data = base + secRva;
        for (DWORD off = 0; off + 4 <= secSize; ++off) {
            if ((secRva + off) % 4 != 0) continue;
            // push ebp (55); mov ebp, esp (8B EC)
            if (data[off] != 0x55 || data[off+1] != 0x8B || data[off+2] != 0xEC) continue;
            // Pre-byte must be a known inter-function padding value.
            if (off > 0) {
                const BYTE p = data[off-1];
                if (p != 0xCC && p != 0x90 && p != 0xC3 && p != 0xC2 && p != 0x00) continue;
            }
            FuncEntry e;
            e.rva     = secRva + off;
            e.address = reinterpret_cast<ULONG_PTR>(base) + e.rva;
            out.push_back(e);
        }
    }
}

} // anonymous namespace

// ===========================================================================
// Public entry point
// ===========================================================================
void DumpFunctionPrototypes() {
    fopen_s(&g_fp, "FableFunctions.log", "w");
    if (!g_fp) return;

    WriteLine("================================================================================");
    WriteLine("  FableAnniversary-Random  |  Function Prototype Dump");
    WriteLine("  Generated by FableModLoader  |  " __DATE__ "  " __TIME__);
    WriteLine("================================================================================");
    WriteLine("");
    WriteLine("  Format:");
    WriteLine("    [ADDRESS]  RVA=0x????????  <SOURCE>  int <callconv> <name>(<params>)");
    WriteLine("");
    WriteLine("  Sources:");
    WriteLine("    [PROLOG]  Found by x86 prolog scan (push ebp; mov ebp, esp).");
    WriteLine("    [RTTI]    Named via MSVC Run-Time Type Information (class names");
    WriteLine("              decoded from type descriptors embedded in the binary).");
    WriteLine("");
    WriteLine("  Prototype approximations (no PDB):");
    WriteLine("    Return type    : always 'int'   (unknown without symbols)");
    WriteLine("    Param types    : always 'int'   (4-byte slot approximation)");
    WriteLine("    Param count    : exact for __stdcall/__thiscall (from 'ret N')");
    WriteLine("                    estimated for __cdecl (from [ebp+N] accesses)");
    WriteLine("                    unknown = '(...)'");
    WriteLine("================================================================================");
    WriteLine("");

    // --- Module ---
    HMODULE hMod = GetModuleHandleA("Fable.exe");
    if (!hMod) hMod = GetModuleHandleA(nullptr);
    if (!hMod) {
        WriteLine("[ERROR] Could not get Fable.exe module handle.");
        fclose(g_fp); g_fp = nullptr; return;
    }

    BYTE*       base      = reinterpret_cast<BYTE*>(hMod);
    const DWORD imageSize =
        reinterpret_cast<IMAGE_NT_HEADERS*>(
            base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew)
        ->OptionalHeader.SizeOfImage;

    WriteLine("  Module base  : 0x%08X", (unsigned)reinterpret_cast<ULONG_PTR>(base));
    WriteLine("  Image size   : 0x%08X  (%u bytes)", (unsigned)imageSize, (unsigned)imageSize);
    WriteLine("");

    // --- RTTI scan ---
    std::unordered_map<DWORD, std::string> rttiNames; // RVA -> label
    CollectRTTINames(base, imageSize, rttiNames);

    // --- Prolog scan; attach RTTI labels ---
    std::vector<FuncEntry> entries;
    ScanCodeSections(base, imageSize, entries);
    for (auto& e : entries) {
        auto it = rttiNames.find(e.rva);
        if (it != rttiNames.end()) e.name = it->second;
    }

    std::sort(entries.begin(), entries.end(),
              [](const FuncEntry& a, const FuncEntry& b){ return a.rva < b.rva; });

    // --- Stats ---
    size_t namedCount = 0;
    for (auto& e : entries) if (!e.name.empty()) ++namedCount;

    WriteLine("  Functions found via prolog scan : %zu", entries.size());
    WriteLine("  Named via RTTI                  : %zu / %zu",
              namedCount, entries.size());
    WriteLine("");
    WriteLine("--------------------------------------------------------------------------------");
    WriteLine("");

    // --- Emit prototypes ---
    for (size_t i = 0; i < entries.size(); ++i) {
        const FuncEntry& e = entries[i];

        size_t funcSize = 8192;
        if (i + 1 < entries.size())
            funcSize = static_cast<size_t>(entries[i+1].rva - e.rva);
        if (funcSize > 8192) funcSize = 8192;

        const FuncAnalysis a = AnalyzeFunction(base + e.rva, funcSize);
        const char* src = rttiNames.count(e.rva) ? "[RTTI]  " : "[PROLOG]";

        const std::string proto = BuildPrototype(e.rva, e.name, a);
        WriteLine("  [0x%08X]  RVA=0x%08X  %s  %s",
                  (unsigned)e.address, (unsigned)e.rva, src, proto.c_str());
    }

    WriteLine("");
    WriteLine("================================================================================");
    WriteLine("  End of dump  |  %zu function(s) listed", entries.size());
    WriteLine("================================================================================");

    fclose(g_fp); g_fp = nullptr;
}
