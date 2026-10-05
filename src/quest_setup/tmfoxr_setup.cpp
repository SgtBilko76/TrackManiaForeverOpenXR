// TMFOXR-Setup: first start of the TrackMania VR container image (WinlatorXR).
//
// The container image ships Wine, TMFOXR and the tested settings but not the
// game, so nothing of Nadeo's is redistributed. On the first start this
// program downloads Nadeo's official TrackMania Nations Forever installer,
// checks its SHA-256, installs it silently to C:\TmForever, makes
// TmForever.exe Large-Address-Aware (otherwise TMFOXR asks for that and closes
// the game on its first start) and adds TMFOXR as the game's d3d9.dll. Every start then copies the current TMFOXR files over
// and launches the game.
//
// Offline: put tmnationsforever_setup.exe into the headset's Download folder
// (D:\) and it is used instead of the download.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <urlmon.h>

#include "../large_address_aware.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kGameDir[] = L"C:\\TmForever";
constexpr wchar_t kInstallerName[] = L"tmnationsforever_setup.exe";
constexpr const wchar_t* kInstallerUrls[] = {
    L"http://files.trackmaniaforever.com/tmnationsforever_setup.exe",
    L"http://files2.trackmaniaforever.com/tmnationsforever_setup.exe",
};
// Nadeo's installer of TrackMania Nations Forever 2.11.26 (530,600,781 bytes).
constexpr char kInstallerSha256[] = "2f659138ed4409da404970841e18f03d29921beaf6a424824c8312ddb20f6355";
// Files beside this program that go into the game folder on every start.
constexpr const wchar_t* kModFiles[] = {L"d3d9.dll", L"TMFOXR.defaults.ini"};

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path);
    return directory.substr(0, directory.find_last_of(L"\\/"));
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void Say(const char* text) {
    std::printf("%s\n", text);
    std::fflush(stdout);
}

// Leaves an error readable on the headset before the window closes.
int Fail(const char* text) {
    std::printf("\nERROR: %s\n\nThis window closes in 60 seconds.\n", text);
    std::fflush(stdout);
    Sleep(60000);
    return 1;
}

std::string Sha256OfFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buffer(1 << 20);
        DWORD read = 0;
        bool ok = true;
        while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read) {
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0) {
                ok = false;
                break;
            }
        }
        unsigned char digest[32]{};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            char text[65]{};
            for (int i = 0; i < 32; ++i) std::snprintf(text + i * 2, 3, "%02x", digest[i]);
            hex = text;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return hex;
}

// Prints the download progress in 5% steps.
class Progress final : public IBindStatusCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (id == IID_IUnknown || id == IID_IBindStatusCallback) {
            *object = static_cast<IBindStatusCallback*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG progress, ULONG total, ULONG, LPCWSTR) override {
        if (!total) return S_OK;
        const int percent = static_cast<int>(100.0 * progress / total);
        if (percent >= lastPercent_ + 5) {
            lastPercent_ = percent - percent % 5;
            std::printf("  %3d%%  (%lu of %lu MB)\n", lastPercent_, progress >> 20, total >> 20);
            std::fflush(stdout);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }

private:
    int lastPercent_ = -5;
};

bool RunAndWait(std::wstring commandLine, const wchar_t* directory) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, directory, &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode == 0;
}

// Finds a verified installer: D:\ first (offline), else a fresh download.
std::wstring ObtainInstaller(const std::wstring& setupDir) {
    const std::wstring offline = std::wstring(L"D:\\") + kInstallerName;
    if (FileExists(offline)) {
        Say("Found tmnationsforever_setup.exe in the Download folder, checking it ...");
        if (Sha256OfFile(offline) == kInstallerSha256) return offline;
        Say("  It does not match Nadeo's installer; downloading instead.");
    }
    const std::wstring target = setupDir + L"\\" + kInstallerName;
    if (FileExists(target) && Sha256OfFile(target) == kInstallerSha256) return target;
    for (const wchar_t* url : kInstallerUrls) {
        std::printf("Downloading TrackMania Nations Forever from Nadeo (530 MB):\n  %ls\n", url);
        std::fflush(stdout);
        DeleteFileW(target.c_str());
        Progress progress;
        if (FAILED(URLDownloadToFileW(nullptr, url, target.c_str(), 0, &progress))) {
            Say("  The download failed.");
            continue;
        }
        Say("Checking the download ...");
        if (Sha256OfFile(target) == kInstallerSha256) return target;
        Say("  The downloaded file is not Nadeo's installer.");
    }
    DeleteFileW(target.c_str());
    return {};
}

// The game folder's d3d9.dll (TMFOXR) must win over DXVK's in system32.
void PreferGameFolderD3D9() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Wine\\AppDefaults\\TmForever.exe\\DllOverrides", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const wchar_t value[] = L"native,builtin";
    RegSetValueExW(key, L"d3d9", 0, REG_SZ, reinterpret_cast<const BYTE*>(value), sizeof(value));
    RegCloseKey(key);
}
} // namespace

int wmain() {
    SetConsoleTitleW(L"TrackMania VR (TMFOXR)");
    const std::wstring setupDir = ModuleDirectory();
    const std::wstring gameExe = std::wstring(kGameDir) + L"\\TmForever.exe";

    if (!FileExists(gameExe)) {
        Say("TrackMania VR - first start\n"
            "===========================\n"
            "TrackMania Nations Forever is free. This container downloads it once\n"
            "from Nadeo's official server and installs it to C:\\TmForever.\n");
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return Fail("COM could not be started.");
        const std::wstring installer = ObtainInstaller(setupDir);
        if (installer.empty()) {
            return Fail("TrackMania could not be downloaded. Check the headset's internet\n"
                        "connection, or put tmnationsforever_setup.exe into the Download folder.");
        }
        Say("Installing TrackMania Nations Forever (about a minute) ...");
        const std::wstring command = L"\"" + installer +
            L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /NOICONS /DIR=\"" + kGameDir + L"\"";
        if (!RunAndWait(command, setupDir.c_str()) || !FileExists(gameExe)) {
            return Fail("The TrackMania installer did not finish.");
        }
        if (installer.rfind(setupDir, 0) == 0) DeleteFileW(installer.c_str());
        Say("TrackMania is installed.\n");
    }
    const tmoxr::laa::PatchResult memoryFix = tmoxr::laa::PatchExecutable(gameExe);
    if (memoryFix.status != tmoxr::laa::PatchStatus::Patched &&
        memoryFix.status != tmoxr::laa::PatchStatus::AlreadyEnabled) {
        return Fail("TmForever.exe could not be made Large-Address-Aware.");
    }

    for (const wchar_t* name : kModFiles) {
        const std::wstring source = setupDir + L"\\" + name;
        const std::wstring target = std::wstring(kGameDir) + L"\\" + name;
        if (FileExists(source) && !CopyFileW(source.c_str(), target.c_str(), FALSE)) {
            return Fail("The VR mod could not be copied into the game folder.");
        }
    }
    PreferGameFolderD3D9();

    Say("Starting TrackMania. The menu appears flat; VR starts with a race.");
    std::wstring command = L"\"" + gameExe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, kGameDir, &startup, &process)) {
        return Fail("TrackMania could not be started.");
    }
    // Hide this window: WinlatorXR reads its frame-sync pixel from the first
    // window it finds. Waiting keeps the shortcut's session open (autoclose).
    if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
