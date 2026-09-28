// WinlatorXR XrAPI backend. Used instead of vr_bridge.cpp when TrackMania runs
// standalone on a Quest/Pico headset inside WinlatorXR.
//
// Protocol (XrAPI 0.5, https://winlatorxr.github.io/xrapi.html):
// - Writing Z:\tmp\xr\version requests the API; WinlatorXR writes Z:\tmp\xr\system.
// - WinlatorXR sends tracking as one ASCII line per headset frame to UDP 7872
//   (7873 for a second client).
// - The game sends "L_HAPTICS R_HAPTICS MODE_VR MODE_3D FOVX FOVY" to UDP 7278.
// - The game renders both eyes side by side into its own window and writes the
//   HMD_SYNC value of the pose it used as the red channel of the top-left pixel.

#include "vr_bridge.h"

#include "controller_input.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace tmoxr {
namespace {
constexpr u_short kTrackingPorts[] = {7872, 7873};
constexpr u_short kModePort = 7278;
constexpr char kApiVersion[] = "0.5";
constexpr wchar_t kDefaultApiDirectory[] = L"Z:\\tmp\\xr";
constexpr ULONGLONG kModeResendMilliseconds = 500;
constexpr DWORD kDefaultSyncWaitMilliseconds = 14;
constexpr ULONGLONG kInputTimeoutMilliseconds = 300;
constexpr float kPi = 3.14159265358979f;

// Field offsets after the leading "clientN" token.
constexpr size_t kLeftThumbstickX = 4;
constexpr size_t kLeftThumbstickY = 5;
constexpr size_t kRightThumbstickX = 13;
constexpr size_t kRightThumbstickY = 14;
constexpr size_t kHeadOrientation = 18;
constexpr size_t kHeadPosition = 22;
constexpr size_t kIpd = 25;
constexpr size_t kFovX = 26;
constexpr size_t kFovY = 27;
constexpr size_t kSync = 28;
constexpr size_t kFloatCount = 29;
// The right thumbstick press opens WinlatorXR's menu (and its VR keyboard);
// holding the left press recenters.
constexpr ULONGLONG kRecenterHoldMilliseconds = 1000;

struct Quaternion { float x, y, z, w; };
struct Vector3 { float x, y, z; };

Quaternion Normalize(const Quaternion& value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
    if (length < 0.000001f) return {0.0f, 0.0f, 0.0f, 1.0f};
    return {value.x / length, value.y / length, value.z / length, value.w / length};
}

Quaternion Conjugate(const Quaternion& value) { return {-value.x, -value.y, -value.z, value.w}; }

Quaternion Multiply(const Quaternion& a, const Quaternion& b) {
    return Normalize({
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z});
}

Vector3 Rotate(const Quaternion& rotation, const Vector3& value) {
    const Vector3 twiceCross{
        2.0f * (rotation.y * value.z - rotation.z * value.y),
        2.0f * (rotation.z * value.x - rotation.x * value.z),
        2.0f * (rotation.x * value.y - rotation.y * value.x)};
    return {
        value.x + rotation.w * twiceCross.x + rotation.y * twiceCross.z - rotation.z * twiceCross.y,
        value.y + rotation.w * twiceCross.y + rotation.z * twiceCross.x - rotation.x * twiceCross.z,
        value.z + rotation.w * twiceCross.z + rotation.x * twiceCross.y - rotation.y * twiceCross.x};
}

struct TrackingSample {
    Quaternion headOrientation{0.0f, 0.0f, 0.0f, 1.0f};
    Vector3 headPosition{};
    float ipd = 0.064f;
    float fovXDegrees = 0.0f;
    float fovYDegrees = 0.0f;
    int sync = 0;
    ControllerState controller;
};

bool ParseTrackingMessage(const char* text, size_t length, TrackingSample& sample) {
    std::array<float, kFloatCount> values{};
    std::string buttons;
    size_t floatCount = 0;
    const char* cursor = text;
    const char* const end = text + length;
    bool first = true;
    while (cursor < end) {
        while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) ++cursor;
        const char* tokenEnd = cursor;
        while (tokenEnd < end && *tokenEnd != ' ' && *tokenEnd != '\t' && *tokenEnd != '\r' && *tokenEnd != '\n') ++tokenEnd;
        if (cursor == tokenEnd) break;
        if (first && (*cursor < '0' || *cursor > '9') && *cursor != '-' && *cursor != '.') {
            // "clientN" prefix.
        } else if (floatCount < kFloatCount) {
            float value = 0.0f;
            const auto result = std::from_chars(cursor, tokenEnd, value);
            if (result.ec != std::errc()) return false;
            values[floatCount++] = value;
        } else if (buttons.empty()) {
            buttons.assign(cursor, tokenEnd);
            break;
        }
        first = false;
        cursor = tokenEnd;
    }
    if (floatCount < kFloatCount) return false;
    sample.headOrientation = Normalize({values[kHeadOrientation], values[kHeadOrientation + 1],
                                        values[kHeadOrientation + 2], values[kHeadOrientation + 3]});
    sample.headPosition = {values[kHeadPosition], values[kHeadPosition + 1], values[kHeadPosition + 2]};
    sample.ipd = values[kIpd];
    sample.fovXDegrees = values[kFovX];
    sample.fovYDegrees = values[kFovY];
    sample.sync = std::clamp(static_cast<int>(std::lround(values[kSync])), 0, 255);
    sample.controller.leftStick[0] = values[kLeftThumbstickX];
    sample.controller.leftStick[1] = values[kLeftThumbstickY];
    sample.controller.rightStick[0] = values[kRightThumbstickX];
    sample.controller.rightStick[1] = values[kRightThumbstickY];
    for (size_t index = 0; index < buttons.size() && index < 32; ++index) {
        if (buttons[index] == 'T') sample.controller.buttons |= 1u << index;
    }
    sample.controller.connected = true;
    return true;
}

std::mutex g_controllerMutex;
ControllerState g_controllerState;

void PublishControllerState(const ControllerState& state) {
    std::lock_guard lock(g_controllerMutex);
    g_controllerState = state;
}

std::wstring ApiDirectory() {
    const DWORD required = GetEnvironmentVariableW(L"TMFOXR_XRAPI_DIR", nullptr, 0);
    if (required > 1) {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"TMFOXR_XRAPI_DIR", value.data(), required);
        value.resize(written);
        if (!value.empty()) return value;
    }
    return kDefaultApiDirectory;
}

DWORD SyncWaitMilliseconds() {
    wchar_t value[16]{};
    if (!GetEnvironmentVariableW(L"TMFOXR_XRAPI_SYNC_WAIT_MS", value, 16)) return kDefaultSyncWaitMilliseconds;
    return static_cast<DWORD>(std::clamp(_wtoi(value), 0, 50));
}
// Menu navigation keys. Driving goes through the virtual joypad, so only
// controls TrackMania does not need while driving are mapped: the right stick
// to the arrow keys, A to Enter and B to Esc. WinlatorXR leaves these buttons
// unmapped by default. Keys are injected with scan codes because TrackMania
// reads the keyboard through DirectInput.
class MenuKeyMapper {
public:
    void Update(const ControllerState& state) {
        const float x = state.rightStick[0];
        const float y = state.rightStick[1];
        Set(Key::Left, StickPressed(Key::Left, -x));
        Set(Key::Right, StickPressed(Key::Right, x));
        Set(Key::Up, StickPressed(Key::Up, y));
        Set(Key::Down, StickPressed(Key::Down, -y));
        Set(Key::Enter, state.Pressed(kButtonA));
        Set(Key::Escape, state.Pressed(kButtonB));
    }

    void ReleaseAll() {
        for (size_t key = 0; key < kKeyCount; ++key) Set(static_cast<Key>(key), false);
    }

private:
    enum class Key : size_t { Left, Right, Up, Down, Enter, Escape };
    static constexpr size_t kKeyCount = 6;
    struct KeyCode { WORD virtualKey; bool extended; };
    static constexpr std::array<KeyCode, kKeyCount> kKeyCodes{{
        {VK_LEFT, true}, {VK_RIGHT, true}, {VK_UP, true}, {VK_DOWN, true},
        {VK_RETURN, false}, {VK_ESCAPE, false}}};
    // Hysteresis keeps a stick resting near the threshold from chattering.
    static constexpr float kStickPress = 0.5f;
    static constexpr float kStickRelease = 0.35f;

    bool StickPressed(Key key, float deflection) const {
        return deflection >= (pressed_[static_cast<size_t>(key)] ? kStickRelease : kStickPress);
    }

    void Set(Key key, bool down) {
        const size_t index = static_cast<size_t>(key);
        if (pressed_[index] == down) return;
        pressed_[index] = down;
        const KeyCode code = kKeyCodes[index];
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = code.virtualKey;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(code.virtualKey, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = KEYEVENTF_SCANCODE | (code.extended ? KEYEVENTF_EXTENDEDKEY : 0) |
                           (down ? 0 : KEYEVENTF_KEYUP);
        SetLastError(ERROR_SUCCESS);
        const UINT sent = SendInput(1, &input, sizeof(input));
        const DWORD sendError = sent ? ERROR_SUCCESS : GetLastError();
        if (!loggedFirstKey_ && down) {
            loggedFirstKey_ = true;
            char title[128]{};
            const HWND foreground = GetForegroundWindow();
            if (foreground) GetWindowTextA(foreground, title, sizeof(title));
            log::Info("XrAPI: first controller key sent (virtual key " + std::to_string(code.virtualKey) +
                ", SendInput=" + std::to_string(sent) + ", error=" + std::to_string(sendError) +
                ", foreground window \"" + title + "\").");
        }
    }

    std::array<bool, kKeyCount> pressed_{};
    bool loggedFirstKey_ = false;
};
// WinlatorXR reads the sync pixel from the first X window it draws, so the
// window layout decides whether it sees the game. Logged once for diagnosis.
std::string DescribeWindow(HWND window) {
    char className[128]{};
    char title[128]{};
    RECT rect{};
    GetClassNameA(window, className, sizeof(className));
    GetWindowTextA(window, title, sizeof(title));
    GetWindowRect(window, &rect);
    const auto xWindow = reinterpret_cast<uintptr_t>(GetPropA(window, "__wine_x11_whole_window"));
    return std::string("class=\"") + className + "\" title=\"" + title + "\" rect=(" +
        std::to_string(rect.left) + "," + std::to_string(rect.top) + ")-(" + std::to_string(rect.right) + "," +
        std::to_string(rect.bottom) + ") x11=" + std::to_string(xWindow);
}

BOOL CALLBACK LogTopLevelWindow(HWND window, LPARAM) {
    if (IsWindowVisible(window)) log::Info("XrAPI window layout: top-level " + DescribeWindow(window));
    return TRUE;
}

void LogWindowLayout() {
    log::Info("XrAPI window layout: desktop " + DescribeWindow(GetDesktopWindow()));
    EnumWindows(&LogTopLevelWindow, 0);
}
} // namespace

struct VrBridge::Impl {
    std::mutex mutex;
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS present{};
    bool eyeSizeDirty = true;
    UINT eyeWidth = 0;
    UINT eyeHeight = 0;

    bool winsockStarted = false;
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in modeTarget{};
    ULONGLONG lastModeSend = 0;
    bool initialized = false;
    bool permanentlyDisabled = false;
    DWORD syncWaitMilliseconds = kDefaultSyncWaitMilliseconds;

    TrackingSample latest{};
    bool haveLatest = false;
    uint64_t receivedSamples = 0;
    uint64_t rejectedSamples = 0;

    bool frameLatched = false;
    int frameSync = -1;
    int lastPresentedSync = -1;
    float frameIpd = 0.064f;

    Quaternion baseOrientation{0.0f, 0.0f, 0.0f, 1.0f};
    Vector3 basePosition{};
    bool haveBase = false;
    ULONGLONG recenterPressStart = 0;
    bool recenterGestureConsumed = false;
    bool recenterOnTrackingJump = true;
    bool verboseDiagnostics = false;
    MenuKeyMapper keyMapper;
    ULONGLONG lastSampleTick = 0;

    HeadPose headPose{};
    bool haveHeadPose = false;
    RenderConfiguration renderConfiguration{};
    bool haveRenderConfiguration = false;

    uint64_t presentedFrames = 0;
    uint64_t syncWaits = 0;
    uint64_t repeatedSyncFrames = 0;

    bool Initialize() {
        if (initialized) return true;
        if (permanentlyDisabled) return false;
        const std::wstring directory = ApiDirectory();
        const std::wstring systemFile = directory + L"\\system";
        if (GetFileAttributesW(systemFile.c_str()) == INVALID_FILE_ATTRIBUTES) {
            log::Warn("WinlatorXR XrAPI not detected (no " + Narrow(systemFile) +
                "); continuing without VR.");
            permanentlyDisabled = true;
            return false;
        }

        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            log::Error("XrAPI: WSAStartup failed.");
            permanentlyDisabled = true;
            return false;
        }
        winsockStarted = true;
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) {
            log::Error("XrAPI: could not create the UDP socket: " + std::to_string(WSAGetLastError()));
            Close();
            permanentlyDisabled = true;
            return false;
        }
        bool bound = false;
        for (const u_short port : kTrackingPorts) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(port);
            if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
                log::Info("XrAPI: listening for WinlatorXR tracking on UDP " + std::to_string(port) + ".");
                bound = true;
                break;
            }
        }
        u_long nonBlocking = 1;
        if (!bound || ioctlsocket(socket, FIONBIO, &nonBlocking) != 0) {
            log::Error("XrAPI: could not bind UDP 7872/7873: " + std::to_string(WSAGetLastError()));
            Close();
            permanentlyDisabled = true;
            return false;
        }
        modeTarget.sin_family = AF_INET;
        modeTarget.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        modeTarget.sin_port = htons(kModePort);

        const std::wstring versionFile = directory + L"\\version";
        const HANDLE file = CreateFileW(versionFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const std::string contents = std::string(kApiVersion) + "\n";
        if (file == INVALID_HANDLE_VALUE ||
            !WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr)) {
            log::Error("XrAPI: could not write " + Narrow(versionFile) + ": " + std::to_string(GetLastError()));
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            Close();
            permanentlyDisabled = true;
            return false;
        }
        CloseHandle(file);

        syncWaitMilliseconds = SyncWaitMilliseconds();
        initialized = true;
        SendMode(true);
        log::Info("XrAPI " + std::string(kApiVersion) + " requested from WinlatorXR; side-by-side stereo, sync wait up to " +
            std::to_string(syncWaitMilliseconds) + " ms.");
        return true;
    }

    static std::string Narrow(const std::wstring& value) {
        std::string output;
        output.reserve(value.size());
        for (const wchar_t character : value) output.push_back(character < 128 ? static_cast<char>(character) : '?');
        return output;
    }

    void SendMode(bool enabled) {
        if (socket == INVALID_SOCKET) return;
        // L_HAPTICS R_HAPTICS MODE_VR MODE_3D FOVX FOVY. A FOV of 0 keeps the headset's own.
        const char* message = enabled ? "0 0 1 1 0 0" : "0 0 0 0 0 0";
        sendto(socket, message, static_cast<int>(std::strlen(message)), 0,
               reinterpret_cast<const sockaddr*>(&modeTarget), sizeof(modeTarget));
        lastModeSend = GetTickCount64();
    }

    void Close() {
        keyMapper.ReleaseAll();
        PublishControllerState({});
        if (socket != INVALID_SOCKET) {
            if (initialized) SendMode(false);
            closesocket(socket);
        }
        socket = INVALID_SOCKET;
        if (winsockStarted) WSACleanup();
        winsockStarted = false;
        initialized = false;
    }

    // Returns true when at least one datagram was read.
    bool Drain() {
        std::array<char, 2048> buffer{};
        bool received = false;
        for (;;) {
            const int length = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (length <= 0) break;
            TrackingSample sample{};
            if (ParseTrackingMessage(buffer.data(), static_cast<size_t>(length), sample)) {
                latest = std::move(sample);
                haveLatest = true;
                PublishControllerState(latest.controller);
                ++receivedSamples;
                received = true;
                lastSampleTick = GetTickCount64();
            } else if (++rejectedSamples <= 3) {
                log::Warn("XrAPI: ignored malformed tracking message: " +
                    std::string(buffer.data(), static_cast<size_t>(std::min(length, 160))));
            }
        }
        return received;
    }

    // Holding the game until the headset publishes a new sync value keeps each
    // rendered frame paired with a fresh pose and stops the emulated CPU from
    // rendering frames the headset will never show.
    void WaitForNewSync() {
        if (!haveLatest || latest.sync != lastPresentedSync || !syncWaitMilliseconds) return;
        ++syncWaits;
        const ULONGLONG deadline = GetTickCount64() + syncWaitMilliseconds;
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket, &readable);
            timeval timeout{0, static_cast<long>((deadline - now) * 1000)};
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) break;
            Drain();
            if (latest.sync != lastPresentedSync) return;
        }
    }

    void UpdateEyeSize() {
        if (!eyeSizeDirty || !device) return;
        IDirect3DSurface9* backBuffer = nullptr;
        D3DSURFACE_DESC description{};
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) return;
        const HRESULT result = backBuffer->GetDesc(&description);
        backBuffer->Release();
        if (FAILED(result) || description.Width < 2 || !description.Height) return;
        eyeWidth = description.Width / 2;
        eyeHeight = description.Height;
        eyeSizeDirty = false;
        log::Info("XrAPI: side-by-side window " + std::to_string(description.Width) + "x" +
            std::to_string(description.Height) + ", " + std::to_string(eyeWidth) + "x" +
            std::to_string(eyeHeight) + " per eye.");
    }

    void UpdateHeadPose(const TrackingSample& sample) {
        // Holding avoids recentering on a stray click while steering.
        bool recenter = false;
        if (sample.controller.Pressed(kLeftThumbstickPress)) {
            const ULONGLONG now = GetTickCount64();
            if (!recenterPressStart) recenterPressStart = now;
            if (!recenterGestureConsumed && now - recenterPressStart >= kRecenterHoldMilliseconds) {
                recenter = true;
                recenterGestureConsumed = true;
            }
        } else {
            recenterPressStart = 0;
            recenterGestureConsumed = false;
        }
        if (!haveBase || recenter) {
            baseOrientation = YawOnly(sample.headOrientation);
            basePosition = sample.headPosition;
            if (haveBase) log::Info("XrAPI: view recentered (left thumbstick held).");
            else log::Info("XrAPI head-pose origin captured; headset motion will now drive the stereo camera.");
            haveBase = true;
        }

        const Quaternion inverseBase = Conjugate(baseOrientation);
        const Quaternion relativeOrientation = Multiply(inverseBase, sample.headOrientation);
        const Vector3 delta{sample.headPosition.x - basePosition.x,
                            sample.headPosition.y - basePosition.y,
                            sample.headPosition.z - basePosition.z};
        Vector3 relativePosition = Rotate(inverseBase, delta);
        if (recenterOnTrackingJump &&
            std::sqrt(relativePosition.x * relativePosition.x + relativePosition.y * relativePosition.y +
                      relativePosition.z * relativePosition.z) > 0.5f) {
            // A system recenter or tracking loss moves the local-space origin.
            basePosition = sample.headPosition;
            relativePosition = {};
            log::Warn("XrAPI: head position jumped by more than 0.5 m; positional tracking was recentered.");
        }
        headPose.position[0] = relativePosition.x;
        headPose.position[1] = relativePosition.y;
        headPose.position[2] = relativePosition.z;
        headPose.orientation[0] = relativeOrientation.x;
        headPose.orientation[1] = relativeOrientation.y;
        headPose.orientation[2] = relativeOrientation.z;
        headPose.orientation[3] = relativeOrientation.w;
        headPose.ipd = frameIpd;
        ++headPose.sample;
        haveHeadPose = true;
    }

    // Recentering keeps the player's pitch and roll relative to gravity.
    static Quaternion YawOnly(const Quaternion& orientation) {
        const Vector3 forward = Rotate(orientation, {0.0f, 0.0f, -1.0f});
        const float yaw = std::atan2(-forward.x, -forward.z);
        return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    }

    void UpdateRenderConfiguration(const TrackingSample& sample) {
        UpdateEyeSize();
        if (!eyeWidth || !eyeHeight || sample.fovXDegrees <= 1.0f || sample.fovYDegrees <= 1.0f) return;
        const float halfX = sample.fovXDegrees * kPi / 360.0f;
        const float halfY = sample.fovYDegrees * kPi / 360.0f;
        auto& eyes = renderConfiguration.eyes;
        if (haveRenderConfiguration && eyes[0].width == eyeWidth && eyes[0].height == eyeHeight &&
            eyes[0].angleRight == halfX && eyes[0].angleUp == halfY) return;
        for (auto& eye : eyes) {
            eye.width = eyeWidth;
            eye.height = eyeHeight;
            eye.angleLeft = -halfX;
            eye.angleRight = halfX;
            eye.angleDown = -halfY;
            eye.angleUp = halfY;
        }
        ++renderConfiguration.sample;
        haveRenderConfiguration = true;
        log::Info("XrAPI: headset FOV " + std::to_string(sample.fovXDegrees) + "x" +
            std::to_string(sample.fovYDegrees) + " degrees, IPD " + std::to_string(sample.ipd) + " m.");
    }

    void BeginFrame() {
        if (!initialized || frameLatched) return;
        Drain();
        WaitForNewSync();
        if (GetTickCount64() - lastModeSend >= kModeResendMilliseconds) SendMode(true);
        // Release held keys when WinlatorXR stops streaming (paused, closed).
        if (haveLatest && GetTickCount64() - lastSampleTick < kInputTimeoutMilliseconds) {
            keyMapper.Update(latest.controller);
        } else {
            keyMapper.ReleaseAll();
            PublishControllerState({});
        }
        if (!haveLatest) return;
        frameLatched = true;
        frameSync = latest.sync;
        if (latest.ipd > 0.04f && latest.ipd < 0.09f) frameIpd = latest.ipd;
        UpdateHeadPose(latest);
        UpdateRenderConfiguration(latest);
    }

    void EndFrame() {
        if (!initialized) return;
        if (presentedFrames == 300) LogWindowLayout();
        if (frameLatched) {
            if (frameSync == lastPresentedSync) ++repeatedSyncFrames;
            lastPresentedSync = frameSync;
        }
        frameLatched = false;
        if (++presentedFrames % 600 == 0 && verboseDiagnostics) {
            log::Info("XrAPI diagnostic: samples received/rejected=" + std::to_string(receivedSamples) + "/" +
                std::to_string(rejectedSamples) + ", sync waits=" + std::to_string(syncWaits) +
                ", frames reusing a sync=" + std::to_string(repeatedSyncFrames) + " of 600.");
            syncWaits = 0;
            repeatedSyncFrames = 0;
        }
    }
};

VrBridge& VrBridge::Instance() { static VrBridge bridge; return bridge; }
VrBridge::~VrBridge() { Shutdown(); }

bool VrBridge::UsesGameWindowAsDisplay() { return true; }

bool VrBridge::GetWindowPresentation(WindowPresentation& presentation) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->initialized || !impl_->frameLatched || impl_->frameSync < 0) return false;
    // Green must stay 0 and alpha non-zero; blue 0 selects the left/SBS target.
    presentation.syncColor = D3DCOLOR_ARGB(255, impl_->frameSync, 0, 0);
    return true;
}

void VrBridge::OnDeviceCreated(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& parameters) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->device) impl_->device->Release();
    impl_->device = device;
    impl_->device->AddRef();
    impl_->present = parameters;
    impl_->eyeSizeDirty = true;
}

bool VrBridge::TryInitialize() {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    return impl_->Initialize();
}

void VrBridge::OnBeginScene() {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->BeginFrame();
}

void VrBridge::OnBeforePresent(IDirect3DDevice9*) {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->EndFrame();
}

void VrBridge::OnBeforeReset() {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->eyeSizeDirty = true;
    impl_->frameLatched = false;
}

void VrBridge::OnTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX&) {}
void VrBridge::OnGameProjection(const D3DMATRIX&) {}
void VrBridge::OnRenderTarget(IDirect3DSurface9*) {}
void VrBridge::OnDraw(bool) {}

// The proxy composes the packed eye target into the window itself.
void VrBridge::SetLeftEyeSurface(IDirect3DSurface9*, HANDLE, uint32_t) {}
void VrBridge::SetRightEyeSurface(IDirect3DSurface9*, HANDLE, uint32_t) {}
void VrBridge::SetUiSurface(IDirect3DSurface9*, HANDLE) {}

bool VrBridge::GetHeadPose(HeadPose& pose) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->haveHeadPose) return false;
    pose = impl_->headPose;
    return true;
}

bool VrBridge::GetRenderConfiguration(RenderConfiguration& configuration) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->haveRenderConfiguration) return false;
    configuration = impl_->renderConfiguration;
    return true;
}

void VrBridge::SetRecenterOnTrackingJump(bool enabled) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    impl_->recenterOnTrackingJump = enabled;
}

void VrBridge::SetVerboseDiagnostics(bool enabled) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    impl_->verboseDiagnostics = enabled;
}

void VrBridge::Shutdown() {
    if (!impl_) return;
    {
        std::scoped_lock lock(impl_->mutex);
        log::Info("XrAPI bridge shutdown requested.");
        impl_->Close();
        if (impl_->device) impl_->device->Release();
        impl_->device = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}
ControllerState GetControllerState() {
    std::lock_guard lock(g_controllerMutex);
    return g_controllerState;
}
} // namespace tmoxr
