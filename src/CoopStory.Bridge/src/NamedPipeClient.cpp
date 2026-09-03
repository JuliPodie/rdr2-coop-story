#include "coopstory/bridge/NamedPipeClient.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace coopstory::bridge {
namespace {

[[nodiscard]] std::string WindowsError(
    const std::string_view operation,
    const DWORD error = ::GetLastError()) {
    LPSTR systemText{};
    const auto length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0U,
        reinterpret_cast<LPSTR>(&systemText),
        0U,
        nullptr);
    std::string message(operation);
    message += " failed (Win32 ";
    message += std::to_string(error);
    message += ")";
    if (length != 0U && systemText != nullptr) {
        message += ": ";
        message.append(systemText, length);
        ::LocalFree(systemText);
    }
    return message;
}

struct HandleCloser final {
    void operator()(void* value) const noexcept {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            ::CloseHandle(static_cast<HANDLE>(value));
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] bool QueryProcessUserSid(
    const HANDLE process,
    std::vector<std::uint8_t>& storage,
    PSID& sid,
    std::string& error) {
    HANDLE tokenRaw{};
    if (!::OpenProcessToken(process, TOKEN_QUERY, &tokenRaw)) {
        error = WindowsError("OpenProcessToken");
        return false;
    }
    UniqueHandle token{tokenRaw};

    DWORD required{};
    (void)::GetTokenInformation(
        static_cast<HANDLE>(token.get()),
        TokenUser,
        nullptr,
        0U,
        &required);
    if (required == 0U || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        error = WindowsError("GetTokenInformation(size)");
        return false;
    }
    storage.resize(required);
    if (!::GetTokenInformation(
            static_cast<HANDLE>(token.get()),
            TokenUser,
            storage.data(),
            required,
            &required)) {
        error = WindowsError("GetTokenInformation(TokenUser)");
        return false;
    }

    const auto* tokenUser =
        reinterpret_cast<const TOKEN_USER*>(storage.data());
    sid = tokenUser->User.Sid;
    if (sid == nullptr || !::IsValidSid(sid)) {
        error = "process token returned an invalid user SID";
        return false;
    }
    return true;
}

[[nodiscard]] HANDLE PipeHandle(const void* value) noexcept {
    return static_cast<HANDLE>(const_cast<void*>(value));
}

}  // namespace

std::wstring BuildCurrentUserPipeName(const std::wstring_view stem) {
    // Include the Windows SID in the local IPC name, matching the sidecar's CurrentUserOnly policy and avoiding a cross-user pipe collision.
    std::vector<std::uint8_t> tokenBytes;
    PSID sid{};
    std::string error;
    if (!QueryProcessUserSid(
            ::GetCurrentProcess(),
            tokenBytes,
            sid,
            error)) {
        throw std::runtime_error(error);
    }

    LPWSTR sidTextRaw{};
    if (!::ConvertSidToStringSidW(sid, &sidTextRaw) ||
        sidTextRaw == nullptr) {
        throw std::runtime_error(WindowsError("ConvertSidToStringSidW"));
    }
    const std::wstring sidText{sidTextRaw};
    ::LocalFree(sidTextRaw);

    std::wstring result{L"\\\\.\\pipe\\"};
    result.append(stem);
    result.push_back(L'.');
    result.append(sidText);
    return result;
}

NamedPipeClient::NamedPipeClient(
    std::wstring pipeName,
    const std::uint32_t connectTimeoutMs,
    const std::uint32_t writeTimeoutMs)
    : pipeName_(std::move(pipeName)),
      connectTimeoutMs_(connectTimeoutMs),
      writeTimeoutMs_(writeTimeoutMs) {}

NamedPipeClient::~NamedPipeClient() {
    Disconnect();
}

bool NamedPipeClient::Connect(std::string& error) {
    error.clear();
    if (IsConnected()) {
        return true;
    }

    // The sidecar can restart while the ASI remains loaded; wait only a bounded interval so the game tick can keep attempting reconnects without hanging.
    if (!::WaitNamedPipeW(pipeName_.c_str(), connectTimeoutMs_)) {
        error = WindowsError("WaitNamedPipeW");
        return false;
    }

    const auto handle = ::CreateFileW(
        pipeName_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0U,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED |
            SECURITY_SQOS_PRESENT |
            SECURITY_IDENTIFICATION,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = WindowsError("CreateFileW(named pipe)");
        return false;
    }
    pipe_ = handle;

    DWORD readMode = PIPE_READMODE_BYTE;
    if (!::SetNamedPipeHandleState(handle, &readMode, nullptr, nullptr)) {
        error = WindowsError("SetNamedPipeHandleState");
        Disconnect();
        return false;
    }
    // A matching pipe name is not enough.
    // Verify the server process runs as this Windows user before allowing any session traffic across local IPC.
    if (!VerifyServerOwner(error)) {
        Disconnect();
        return false;
    }
    decoder_.Reset();
    return true;
}

void NamedPipeClient::Disconnect() noexcept {
    // Cancel outstanding overlapped I/O before closing so a late completion cannot write through a handle that has been recycled by Windows.
    if (!IsConnected()) {
        return;
    }
    const auto handle = PipeHandle(pipe_);
    (void)::CancelIoEx(handle, nullptr);
    (void)::CloseHandle(handle);
    pipe_ = nullptr;
    decoder_.Reset();
}

bool NamedPipeClient::IsConnected() const noexcept {
    return pipe_ != nullptr && PipeHandle(pipe_) != INVALID_HANDLE_VALUE;
}

bool NamedPipeClient::VerifyServerOwner(std::string& error) const {
    ULONG serverProcessId{};
    if (!::GetNamedPipeServerProcessId(
            PipeHandle(pipe_),
            &serverProcessId)) {
        error = WindowsError("GetNamedPipeServerProcessId");
        return false;
    }

    UniqueHandle serverProcess{
        ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            serverProcessId)};
    if (serverProcess.get() == nullptr) {
        error = WindowsError("OpenProcess(named-pipe server)");
        return false;
    }

    std::vector<std::uint8_t> ownStorage;
    std::vector<std::uint8_t> serverStorage;
    PSID ownSid{};
    PSID serverSid{};
    if (!QueryProcessUserSid(
            ::GetCurrentProcess(),
            ownStorage,
            ownSid,
            error) ||
        !QueryProcessUserSid(
            static_cast<HANDLE>(serverProcess.get()),
            serverStorage,
            serverSid,
            error)) {
        return false;
    }
    if (!::EqualSid(ownSid, serverSid)) {
        error =
            "named-pipe server belongs to a different Windows user; "
            "connection refused";
        return false;
    }
    return true;
}

bool NamedPipeClient::WriteAll(
    const std::uint8_t* data,
    const std::size_t size,
    std::string& error) {
    std::size_t offset{};
    // Named pipes are byte streams: complete the full encoded frame even when Windows performs one write in multiple chunks.
    while (offset < size) {
        const auto chunkSize = static_cast<DWORD>(
            std::min<std::size_t>(
                size - offset,
                static_cast<std::size_t>(MAXDWORD)));
        UniqueHandle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (event.get() == nullptr) {
            error = WindowsError("CreateEventW(named-pipe write)");
            return false;
        }

        OVERLAPPED operation{};
        operation.hEvent = static_cast<HANDLE>(event.get());
        DWORD written{};
        const auto completed = ::WriteFile(
            PipeHandle(pipe_),
            data + offset,
            chunkSize,
            &written,
            &operation);
        if (!completed) {
            const auto writeError = ::GetLastError();
            if (writeError != ERROR_IO_PENDING) {
                error = WindowsError("WriteFile(named pipe)", writeError);
                return false;
            }
            const auto wait = ::WaitForSingleObject(
                static_cast<HANDLE>(event.get()),
                writeTimeoutMs_);
            if (wait != WAIT_OBJECT_0) {
                (void)::CancelIoEx(PipeHandle(pipe_), &operation);
                (void)::WaitForSingleObject(
                    static_cast<HANDLE>(event.get()),
                    INFINITE);
                error = wait == WAIT_TIMEOUT
                            ? "named-pipe write timed out"
                            : WindowsError("WaitForSingleObject(named pipe)");
                return false;
            }
            if (!::GetOverlappedResult(
                    PipeHandle(pipe_),
                    &operation,
                    &written,
                    FALSE)) {
                error = WindowsError("GetOverlappedResult(named-pipe write)");
                return false;
            }
        }
        if (written == 0U) {
            error = "named-pipe write completed without progress";
            return false;
        }
        offset += written;
    }
    return true;
}

bool NamedPipeClient::Send(const Frame& frame, std::string& error) {
    error.clear();
    if (!IsConnected()) {
        error = "named pipe is not connected";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    try {
        bytes = FrameCodec::Encode(frame);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    // A partial failed frame poisons this stream; drop it and reconnect rather than letting the next frame be decoded against an invalid byte boundary.
    if (!WriteAll(bytes.data(), bytes.size(), error)) {
        Disconnect();
        return false;
    }
    return true;
}

std::vector<Frame> NamedPipeClient::Poll(std::string& error) {
    std::vector<Frame> frames;
    error.clear();
    if (!IsConnected()) {
        return frames;
    }

    // Drain every currently available chunk during this game tick, while the streaming decoder preserves incomplete bytes for the next poll.
    for (;;) {
        DWORD available{};
        if (!::PeekNamedPipe(
                PipeHandle(pipe_),
                nullptr,
                0U,
                nullptr,
                &available,
                nullptr)) {
            error = WindowsError("PeekNamedPipe");
            Disconnect();
            return frames;
        }
        if (available == 0U) {
            break;
        }

        std::array<std::uint8_t, 64U * 1024U> bytes{};
        const auto requested = std::min<DWORD>(
            available,
            static_cast<DWORD>(bytes.size()));
        UniqueHandle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (event.get() == nullptr) {
            error = WindowsError("CreateEventW(named-pipe read)");
            Disconnect();
            return frames;
        }

        OVERLAPPED operation{};
        operation.hEvent = static_cast<HANDLE>(event.get());
        DWORD bytesRead{};
        const auto completed = ::ReadFile(
            PipeHandle(pipe_),
            bytes.data(),
            requested,
            &bytesRead,
            &operation);
        if (!completed) {
            const auto readError = ::GetLastError();
            if (readError != ERROR_IO_PENDING) {
                error = WindowsError("ReadFile(named pipe)", readError);
                Disconnect();
                return frames;
            }
            const auto wait = ::WaitForSingleObject(
                static_cast<HANDLE>(event.get()),
                writeTimeoutMs_);
            if (wait != WAIT_OBJECT_0) {
                (void)::CancelIoEx(PipeHandle(pipe_), &operation);
                (void)::WaitForSingleObject(
                    static_cast<HANDLE>(event.get()),
                    INFINITE);
                error = wait == WAIT_TIMEOUT
                            ? "named-pipe read timed out"
                            : WindowsError("WaitForSingleObject(named pipe)");
                Disconnect();
                return frames;
            }
            if (!::GetOverlappedResult(
                    PipeHandle(pipe_),
                    &operation,
                    &bytesRead,
                    FALSE)) {
                error = WindowsError("GetOverlappedResult(named-pipe read)");
                Disconnect();
                return frames;
            }
        }
        if (bytesRead == 0U) {
            error = "named-pipe peer disconnected";
            Disconnect();
            return frames;
        }

        decoder_.Append(
            std::span<const std::uint8_t>{bytes.data(), bytesRead});
        while (auto frame = decoder_.Pop()) {
            frames.push_back(std::move(*frame));
        }
        if (decoder_.HasError()) {
            error = decoder_.Error();
            Disconnect();
            return frames;
        }
    }
    return frames;
}

}  // namespace coopstory::bridge
