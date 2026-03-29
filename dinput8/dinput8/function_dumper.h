// =============================================================================
// Author       : Yaranorgoth
// Description  : Dumps all function prototypes found in Fable.exe.
//                Uses DbgHelp to enumerate symbols and demangle decorated
//                names, producing a human-readable prototype list similar
//                to the output style of UE4SS.
//
// Output file  : FableFunctions.log (written next to the game EXE)
//
// Notes        : Symbol resolution is best-effort.
//                - Only PE-exported symbols are available.
//                - Decorated names are undecorated with UNDNAME_COMPLETE
//                  to recover return type and parameter list.
// =============================================================================

#pragma once

// Dumps all function prototypes from Fable.exe into FableFunctions.log.
// Call once after the process has fully initialised.
void DumpFunctionPrototypes();
