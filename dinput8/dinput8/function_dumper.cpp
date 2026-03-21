// =============================================================================
// Project      : FableAnniversary-Random
// File         : function_dumper.cpp
// Author       : FableModLoader
// Created      : 2026-03-15
// Description  : Dumps all function prototypes found in Fable.exe.
//
// Strategy (no PDB available):
//   1. Scan executable sections for x86 MSVC function prologues (55 8B EC).
//   2. For each function, analyse the body to recover:
//        - Calling convention  : detected from epilogue pattern
//            * 5D C2 NN NN  (pop ebp; ret N)  -> __stdcall, N bytes of params
//            * 5D C3        (pop ebp; ret)     -> __cdecl
//            * early 89 4D ??               -> __thiscall (ECX = this saved)
//        - Parameter count     :
//            * stdcall  : ret N  -> N/4 parameters (exact, by ABI)
//            * cdecl    : scan [ebp+N] accesses (0x08..0x4C) for highest N
//            * thiscall : same as stdcall for explicit params; +1 implicit this
//        - Return type         : "int" (cannot determine without PDB)
//        - Parameter types     : "int" for each slot (best-effort
//        approximation)
//   3. Optional: merge names from export table or DbgHelp/PDB if present.
//
// Output : FableFunctions.log in the game working directory.
// =============================================================================

#include "function_dumper.h"
#include "pch.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

// DbgHelp included with UNICODE suppressed so TCHAR macros map to narrow APIs.
#ifdef UNICODE
#define _FML_RESTORE_UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#define _FML_RESTORE__UNICODE
#undef _UNICODE
#endif
#include <dbghelp.h>
#ifdef _FML_RESTORE_UNICODE
#define UNICODE
#undef _FML_RESTORE_UNICODE
#endif
#ifdef _FML_RESTORE__UNICODE
#define _UNICODE
#undef _FML_RESTORE__UNICODE
#endif
#pragma comment(lib, "dbghelp.lib")

// ---------------------------------------------------------------------------
namespace {
// ---------------------------------------------------------------------------

struct FuncEntry {
  DWORD rva;
  ULONG_PTR address;
  std::string name; // from export table or PDB; empty if unknown
};

// Result of static analysis of one function body.
struct FuncAnalysis {
  const char *callConv = "__cdecl"; // "__cdecl" | "__stdcall" | "__thiscall"
  int paramCount = -1;              // -1 = unknown; 0+ = count of stack params
  bool isThiscall = false;          // true -> add implicit "void* this" first
};

FILE *g_fp = nullptr;

void WriteLine(const char *fmt, ...) {
  if (!g_fp)
    return;
  va_list a;
  va_start(a, fmt);
  vfprintf(g_fp, fmt, a);
  va_end(a);
  fputc('\n', g_fp);
}

// ---------------------------------------------------------------------------
// Name demangling
// ---------------------------------------------------------------------------
static std::string Undecorate(const char *sym) {
  char buf[4096];
  if (UnDecorateSymbolName(sym, buf, sizeof(buf), UNDNAME_COMPLETE) != 0)
    return std::string(buf);
  return std::string(sym);
}

// ---------------------------------------------------------------------------
// PE export table -> RVA-to-name map
// ---------------------------------------------------------------------------
static void CollectExportNames(BYTE *base, DWORD imageSize,
                               std::unordered_map<DWORD, std::string> &out) {
  auto *dh = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
  if (dh->e_magic != IMAGE_DOS_SIGNATURE)
    return;
  auto *nh = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dh->e_lfanew);
  if (nh->Signature != IMAGE_NT_SIGNATURE)
    return;
  auto &ed = nh->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!ed.VirtualAddress || ed.VirtualAddress >= imageSize)
    return;
  auto *exp =
      reinterpret_cast<IMAGE_EXPORT_DIRECTORY *>(base + ed.VirtualAddress);
  auto *fnRva = reinterpret_cast<DWORD *>(base + exp->AddressOfFunctions);
  auto *nmRva = reinterpret_cast<DWORD *>(base + exp->AddressOfNames);
  auto *ords = reinterpret_cast<WORD *>(base + exp->AddressOfNameOrdinals);
  for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
    WORD ord = ords[i];
    if (ord >= exp->NumberOfFunctions)
      continue;
    DWORD rva = fnRva[ord];
    if (!rva || rva >= imageSize)
      continue;
    // Skip forwarder RVAs
    if (rva >= ed.VirtualAddress && rva < ed.VirtualAddress + ed.Size)
      continue;
    const char *name = reinterpret_cast<const char *>(base + nmRva[i]);
    out[rva] = Undecorate(name);
  }
}

// ---------------------------------------------------------------------------
// x86 function body analysis
//
// Calling convention detection
// ─────────────────────────────
//   Thiscall : the compiler saves ECX ("this") to a local variable right
//              after the prolog.  Pattern: 89 4D ?? where ?? >= 0x80
//              (negative offset from EBP = local slot).
//
//   Stdcall  : the function self-cleans the stack → epilogue is
//              pop ebp (5D) / C3 → wait, C2 NN NN = ret N.
//              5D C2 NN NN  or  C9 C2 NN NN  (leave; ret N).
//
//   Cdecl    : caller cleans; epilogue ends with plain C3 (ret 0).
//              5D C3  or  C9 C3.
//
// Parameter count detection
// ─────────────────────────
//   Stdcall/thiscall : `ret N` → N bytes / 4 = parameter count (exact).
//   Cdecl            : scan for [ebp + disp8] accesses (ModRM 0x45,0x4D,…).
//                      The largest positive disp8 seen is the last param slot:
//                        paramCount = (maxDisp - 4) / 4
//                      [ebp+8]=p1, [ebp+0xC]=p2, [ebp+0x10]=p3, …
// ---------------------------------------------------------------------------
static FuncAnalysis AnalyzeFunction(const BYTE *start, size_t maxBytes) {
  FuncAnalysis r;
  if (maxBytes < 4)
    return r;
  const size_t sz = (maxBytes < 8192) ? maxBytes : 8192;

  // --- 1. Thiscall detection (scan first ~48 bytes after prolog) ---
  for (size_t i = 3; i + 2 < sz && i < 48; ++i) {
    BYTE b = start[i];
    // 89 4D ?? where ?? >= 0x80 → mov [ebp - N], ecx  (save 'this')
    if (b == 0x89 && start[i + 1] == 0x4D && start[i + 2] >= 0x80) {
      r.isThiscall = true;
      r.callConv = "__thiscall";
      break;
    }
    if (b == 0xCC)
      break; // hit int3 padding → past end of function
  }

  // --- 2. Epilogue scan to get calling convention and ret-N value ---
  // We track the largest ret-N we see (covers functions with multiple exits).
  int bestRetN = -1; // -1=not found, 0=plain ret, N=ret N bytes

  for (size_t i = 1; i + 1 < sz; ++i) {
    const BYTE prev = start[i - 1];
    const BYTE cur = start[i];
    const bool epilogPrev =
        (prev == 0x5D /*pop ebp*/ || prev == 0xC9 /*leave*/);

    // plain ret → cdecl or thiscall with 0 explicit stack params
    if (epilogPrev && cur == 0xC3) {
      if (bestRetN < 0)
        bestRetN = 0;
    }
    // ret N → stdcall / thiscall with N bytes of explicit stack params
    if (epilogPrev && cur == 0xC2 && i + 2 < sz) {
      WORD n = *reinterpret_cast<const WORD *>(start + i + 1);
      if (n <= 256 && (n % 4) == 0 && (int)n > bestRetN) {
        bestRetN = (int)n;
        if (!r.isThiscall)
          r.callConv = "__stdcall";
      }
    }
  }

  // Param count from ret N (high confidence)
  if (bestRetN > 0) {
    r.paramCount = bestRetN / 4;
    return r;
  }

  // --- 3. [ebp + disp8] scan for cdecl / thiscall param count ---
  // ModRM bytes that encode [ebp + disp8] for any destination register:
  static const BYTE kEbpModRM[] = {0x45, 0x4D, 0x55, 0x5D,
                                   0x65, 0x6D, 0x75, 0x7D};
  int maxDisp = 0;
  for (size_t i = 0; i + 2 < sz; ++i) {
    BYTE modrm = start[i + 1];
    bool isEbp = false;
    for (BYTE m : kEbpModRM) {
      if (modrm == m) {
        isEbp = true;
        break;
      }
    }
    if (!isEbp)
      continue;
    BYTE disp = start[i + 2];
    // Positive offset in the param area: 0x08..0x4C (covers up to 16 params)
    if (disp >= 0x08 && disp <= 0x4C && (disp % 4) == 0 && (int)disp > maxDisp)
      maxDisp = (int)disp;
  }
  if (maxDisp >= 8) {
    int count = (maxDisp - 4) / 4; // [ebp+8]=1, [ebp+0xC]=2, …
    if (count >= 1 && count <= 12)
      r.paramCount = count;
  }

  return r;
}

// ---------------------------------------------------------------------------
// Build a human-readable prototype string from the analysis results
// ---------------------------------------------------------------------------
static std::string BuildPrototype(DWORD rva, const std::string &knownName,
                                  const FuncAnalysis &a) {
  // If we already have a fully-decorated prototype from PDB (contains '('),
  // return it directly.
  if (!knownName.empty() && knownName.find('(') != std::string::npos)
    return knownName;

  // Function name: known symbol name or sub_XXXXXX
  std::string fn;
  if (!knownName.empty()) {
    fn = knownName;
  } else {
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "sub_%X", (unsigned)rva);
    fn = tmp;
  }

  // Return type: always "int" — cannot determine without PDB.
  std::string proto = "int " + std::string(a.callConv) + " " + fn + "(";

  const int sp = (a.paramCount > 0) ? a.paramCount : 0;

  if (a.isThiscall) {
    // Implicit 'this' in ECX + explicit stack params
    proto += "void* this";
    for (int i = 0; i < sp; ++i)
      proto += ", int param_" + std::to_string(i + 1);
  } else if (a.paramCount < 0) {
    proto += "..."; // cdecl, param count unknown
  } else if (sp == 0) {
    proto += "void";
  } else {
    for (int i = 0; i < sp; ++i) {
      if (i)
        proto += ", ";
      proto += "int param_" + std::to_string(i + 1);
    }
  }

  proto += ")";
  return proto;
}

// ---------------------------------------------------------------------------
// Prolog scanner: walk all executable sections, record 55 8B EC hits
// ---------------------------------------------------------------------------
static void ScanCodeSections(BYTE *base, DWORD imageSize,
                             std::vector<FuncEntry> &out) {
  auto *dh = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
  if (dh->e_magic != IMAGE_DOS_SIGNATURE)
    return;
  auto *nh = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dh->e_lfanew);
  if (nh->Signature != IMAGE_NT_SIGNATURE)
    return;
  IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nh);
  WORD numSec = nh->FileHeader.NumberOfSections;

  for (WORD s = 0; s < numSec; ++s) {
    IMAGE_SECTION_HEADER &sec = sh[s];
    if (!(sec.Characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)))
      continue;
    DWORD secRva = sec.VirtualAddress;
    DWORD secSize =
        sec.Misc.VirtualSize ? sec.Misc.VirtualSize : sec.SizeOfRawData;
    if (secRva + secSize > imageSize)
      secSize = imageSize - secRva;
    if (secSize < 4)
      continue;

    BYTE *data = base + secRva;
    for (DWORD off = 0; off + 4 <= secSize; off++) {
      if ((secRva + off) % 4 != 0)
        continue;
      // Primary prolog: push ebp (55); mov ebp, esp (8B EC)
      if (data[off] != 0x55 || data[off + 1] != 0x8B || data[off + 2] != 0xEC)
        continue;
      // Pre-byte padding guard
      if (off > 0) {
        BYTE p = data[off - 1];
        if (p != 0xCC && p != 0x90 && p != 0xC3 && p != 0xC2 && p != 0x00)
          continue;
      }
      FuncEntry e;
      e.rva = secRva + off;
      e.address = reinterpret_cast<ULONG_PTR>(base) + e.rva;
      out.push_back(e);
    }
  }
}

// ---------------------------------------------------------------------------
// Optional DbgHelp / PDB symbol enumeration
// ---------------------------------------------------------------------------
static BOOL CALLBACK DbgCb(PSYMBOL_INFO pSym, ULONG, PVOID ctx) {
  if (pSym->Tag != 5 && pSym->Tag != 10)
    return TRUE; // 5=Function, 10=PublicSymbol
  if (!pSym->Address)
    return TRUE;
  auto *out = reinterpret_cast<std::vector<FuncEntry> *>(ctx);
  FuncEntry e;
  e.address = static_cast<ULONG_PTR>(pSym->Address);
  e.rva = static_cast<DWORD>(
      pSym->Address - reinterpret_cast<ULONG_PTR>(GetModuleHandleA(nullptr)));
  e.name = Undecorate(pSym->Name);
  out->push_back(e);
  return TRUE;
}

static void CollectDbgHelpSymbols(HMODULE hMod, std::vector<FuncEntry> &out) {
  HANDLE hProc = GetCurrentProcess();
  ULONG_PTR base = reinterpret_cast<ULONG_PTR>(hMod);
  char exePath[MAX_PATH] = {};
  GetModuleFileNameA(hMod, exePath, MAX_PATH);
  char *sl = strrchr(exePath, '\\');
  if (sl)
    *sl = '\0';
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                SYMOPT_INCLUDE_32BIT_MODULES);
  if (!SymInitialize(hProc, exePath, FALSE))
    return;
  DWORD64 symBase = SymLoadModuleEx(hProc, nullptr, exePath, nullptr,
                                    (DWORD64)base, 0, nullptr, 0);
  if (symBase || GetLastError() == ERROR_SUCCESS)
    SymEnumSymbols(hProc, symBase, "*", DbgCb, &out);
  SymCleanup(hProc);
}

} // anonymous namespace

// ===========================================================================
// Public entry point
// ===========================================================================
void DumpFunctionPrototypes() {
  fopen_s(&g_fp, "FableFunctions.log", "w");
  if (!g_fp)
    return;

  WriteLine("=================================================================="
            "==============");
  WriteLine("  FableAnniversary-Random  |  Function Prototype Dump");
  WriteLine("  Generated by FableModLoader  |  " __DATE__ "  " __TIME__);
  WriteLine("=================================================================="
            "==============");
  WriteLine("");
  WriteLine("  Format:");
  WriteLine("    [ADDRESS]  RVA=0x????????  <SOURCE>  int <callconv> "
            "<name>(<params>)");
  WriteLine("");
  WriteLine("  Notes (no PDB available):");
  WriteLine(
      "    - Return type is always 'int' (cannot infer without symbols).");
  WriteLine(
      "    - Parameter types are always 'int' (4-byte slot approximation).");
  WriteLine(
      "    - Calling convention and param COUNT are inferred from the binary.");
  WriteLine(
      "        __stdcall  : param count exact  (from 'ret N' in epilogue)");
  WriteLine("        __thiscall : param count exact  (ECX=this + 'ret N')");
  WriteLine(
      "        __cdecl    : param count estimated (from [ebp+N] accesses)");
  WriteLine(
      "        (...)      : param count unknown (cdecl, no [ebp+N] evidence)");
  WriteLine("=================================================================="
            "==============");
  WriteLine("");

  // --- Get module ---
  HMODULE hMod = GetModuleHandleA("Fable.exe");
  if (!hMod)
    hMod = GetModuleHandleA(nullptr);
  if (!hMod) {
    WriteLine("[ERROR] Could not get Fable.exe module handle.");
    fclose(g_fp);
    g_fp = nullptr;
    return;
  }

  BYTE *base = reinterpret_cast<BYTE *>(hMod);
  DWORD imageSize =
      reinterpret_cast<IMAGE_NT_HEADERS *>(
          base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew)
          ->OptionalHeader.SizeOfImage;

  WriteLine("  Module base  : 0x%08X",
            (unsigned)reinterpret_cast<ULONG_PTR>(base));
  WriteLine("  Image size   : 0x%08X  (%u bytes)", (unsigned)imageSize,
            (unsigned)imageSize);
  WriteLine("");

  // --- Collect symbol names from export table & optional PDB ---
  std::unordered_map<DWORD, std::string> knownNames;
  CollectExportNames(base, imageSize, knownNames);

  std::vector<FuncEntry> pdbEntries;
  CollectDbgHelpSymbols(hMod, pdbEntries);
  for (auto &e : pdbEntries)
    if (!e.name.empty())
      knownNames.emplace(e.rva, e.name);

  // --- Prolog scan ---
  std::vector<FuncEntry> entries;
  ScanCodeSections(base, imageSize, entries);
  for (auto &e : entries) {
    auto it = knownNames.find(e.rva);
    if (it != knownNames.end())
      e.name = it->second;
  }

  // Sort by address (already sorted from section scan, but just in case)
  std::sort(
      entries.begin(), entries.end(),
      [](const FuncEntry &a, const FuncEntry &b) { return a.rva < b.rva; });

  // --- Stats ---
  WriteLine("  Functions found via prolog scan : %zu", entries.size());
  WriteLine("  Of which named (export/PDB)     : %zu", knownNames.size());
  WriteLine("");
  WriteLine("------------------------------------------------------------------"
            "--------------");
  WriteLine("");

  // --- Emit one prototype per function ---
  for (size_t i = 0; i < entries.size(); ++i) {
    const FuncEntry &e = entries[i];

    // Compute the safe scan window for this function (up to next function
    // start)
    size_t funcSize = 8192;
    if (i + 1 < entries.size())
      funcSize = static_cast<size_t>(entries[i + 1].rva - e.rva);
    if (funcSize > 8192)
      funcSize = 8192;

    const BYTE *funcPtr = base + e.rva;
    FuncAnalysis a = AnalyzeFunction(funcPtr, funcSize);

    // Source tag
    const char *src = (!pdbEntries.empty() && !e.name.empty()) ? "[PDB]   "
                      : (!e.name.empty())                      ? "[EXPORT]"
                                                               : "[PROLOG]";

    std::string proto = BuildPrototype(e.rva, e.name, a);

    WriteLine("  [0x%08X]  RVA=0x%08X  %s  %s", (unsigned)e.address,
              (unsigned)e.rva, src, proto.c_str());
  }

  WriteLine("");
  WriteLine("=================================================================="
            "==============");
  WriteLine("  End of dump  |  %zu function(s) listed", entries.size());
  WriteLine("=================================================================="
            "==============");

  fclose(g_fp);
  g_fp = nullptr;
}
