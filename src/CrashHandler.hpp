#pragma once
// =============================================================================
// CrashHandler.hpp — Cross-platform native crash capture
//
// Captures fatal signals (POSIX) or unhandled exceptions (Windows) and writes
// a structured crash report to the user-data directory before terminating.
//
// Supported platforms:
//   - Windows (x86/x64): SetUnhandledExceptionFilter + MiniDumpWriteDump
//   - Linux  (x86/x64):  sigaction + backtrace()
//   - Android (ARM32/64): sigaction + _Unwind_Backtrace
//
// Usage:
//   CrashHandler::Init();      // call once, after GamePaths::Init()
//   ...
//   CrashHandler::Shutdown();  // call before exit (restores default handlers)
//
// Crash reports are written to:
//   GamePaths::GetUserPath() + "crash/" + "crash_YYYYMMDD_HHMMSS_<pid>.txt"
//   On Windows, a .dmp minidump is also written alongside the text report.
// =============================================================================

namespace th06::CrashHandler
{

// Register crash handlers. Must be called after GamePaths::Init().
void Init();

// Restore default signal/exception handlers and clean up.
void Shutdown();

} // namespace th06::CrashHandler
