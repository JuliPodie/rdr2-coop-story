#include "ScriptHookSdkFacade.hpp"

#include "coopstory/bridge/BridgeRuntime.hpp"
#include "coopstory/bridge/NamedPipeClient.hpp"
#include "coopstory/bridge/VersionGate.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <main.h>

#include <cstdint>
#include <exception>
#include <string>

namespace {

// Global objects for the ASI plugin entry point.
// Windows/ScriptHook call this layer each game tick; it creates the Bridge and connects it to the Sidecar.
HMODULE g_module{};
DWORD g_lastNativeExceptionCode{};
std::uintptr_t g_lastNativeExceptionAddress{};

LONG CaptureNativeException(
    EXCEPTION_POINTERS* exception) noexcept {
    if (exception != nullptr &&
        exception->ExceptionRecord != nullptr) {
        g_lastNativeExceptionCode =
            exception->ExceptionRecord->ExceptionCode;
        g_lastNativeExceptionAddress =
            reinterpret_cast<std::uintptr_t>(
                exception->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ScriptHook reports a generic id=31 popup when an access violation escapes ScriptMain.
// C++ catch blocks compiled with /EHsc do not catch SEH raised by a native call, so keep the native-facing tick behind a tiny SEH boundary and persist the exact exception before ending the session cleanly.
bool TickWithNativeExceptionBoundary(
    coopstory::bridge::BridgeRuntime* runtime) noexcept {
    __try {
        runtime->Tick();
        return true;
    } __except (CaptureNativeException(
        GetExceptionInformation())) {
        return false;
    }
}

void ScriptMain() {
    using namespace coopstory::bridge;
    sdk::ScriptHookSdkFacade facade;

    try {
        const auto identity = ProbeCurrentGameIdentity();
        if (!identity.succeeded) {
            facade.Log(identity.error);
            return;
        }

        NamedPipeClient pipe;
        BridgeRuntime runtime{facade, pipe};
        std::string startMessage;
        if (!runtime.Start(identity.identity, startMessage)) {
            facade.Log(startMessage);
            return;
        }

        while (runtime.IsActive() && !runtime.ShouldUnload()) {
            if (!TickWithNativeExceptionBoundary(&runtime)) {
                facade.Log(
                    "[FATAL][SEH] guarded native exception code=" +
                    std::to_string(g_lastNativeExceptionCode) +
                    ", address=" +
                    std::to_string(g_lastNativeExceptionAddress) +
                    ", stage=" +
                    std::string{runtime.LastTickStage()} +
                    "; native cleanup intentionally skipped to preserve diagnostics and avoid a second access violation");
                runtime.AbortAfterNativeException();
                facade.AbandonNativeCleanupAfterFatal();
                return;
            }
            facade.WaitForNextTick();
        }
        runtime.Stop("ASI script thread ended");
    } catch (const std::exception& exception) {
        facade.Log(exception.what());
    } catch (...) {
        facade.Log("unknown fatal exception in ASI script thread");
    }
}

}  // namespace

// The private-package builder requires this exported capability marker.
// A structurally valid ASI compiled with native bindings disabled would load fail-closed and could otherwise be mistaken for the playable test bridge.
// Keeping the safe CMake option default OFF remains intentional; only the explicit private-validation presets enable and emit this marker.
extern "C" __declspec(dllexport) const char*
CoopStoryNativeBindingsCapability() noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    return "COOPSTORY_NATIVE_BINDINGS_ENABLED_V1";
#else
    return "COOPSTORY_NATIVE_BINDINGS_DISABLED_V1";
#endif
}

BOOL APIENTRY DllMain(
    const HMODULE module,
    const DWORD reason,
    LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        ::DisableThreadLibraryCalls(module);
        scriptRegister(module, ScriptMain);
    } else if (reason == DLL_PROCESS_DETACH) {
        scriptUnregister(g_module);
    }
    return TRUE;
}
