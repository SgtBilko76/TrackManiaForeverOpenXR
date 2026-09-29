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
#include "runtime_paths.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
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

// TMFOXR_XRAPI_FOV overrides the square field of view in degrees.
float SquareFovOverride() {
    wchar_t value[16]{};
    if (!GetEnvironmentVariableW(L"TMFOXR_XRAPI_FOV", value, 16)) return 0.0f;
    return std::clamp(static_cast<float>(_wtof(value)), 60.0f, 140.0f);
}

DWORD SyncWaitMilliseconds() {
    wchar_t value[16]{};
    if (!GetEnvironmentVariableW(L"TMFOXR_XRAPI_SYNC_WAIT_MS", value, 16)) return kDefaultSyncWaitMilliseconds;
    return static_cast<DWORD>(std::clamp(_wtoi(value), 0, 50));
}
// Menu navigation keys: in menus (flat screen) the left stick sends the arrow
// keys; in races it steers through the virtual joypad instead. A sends Enter
// and B Esc. WinlatorXR leaves these buttons unmapped by default. Keys are
// injected with scan codes because TrackMania reads the keyboard through
// DirectInput.
class MenuKeyMapper {
public:
    void Update(const ControllerState& state, bool menu) {
        const float x = menu ? state.leftStick[0] : 0.0f;
        const float y = menu ? state.leftStick[1] : 0.0f;
        const ULONGLONG now = GetTickCount64();
        Tap(Key::Left, -x, now);
        Tap(Key::Right, x, now);
        Tap(Key::Up, y, now);
        Tap(Key::Down, -y, now);
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

    // A stick direction sends single key taps: one on deflection, then
    // repeats after 500 ms every 250 ms. Holding the key down made the game's
    // key repeat race through menu entries.
    static constexpr ULONGLONG kTapLength = 40;
    static constexpr ULONGLONG kFirstRepeat = 500;
    static constexpr ULONGLONG kRepeatInterval = 250;

    void Tap(Key key, float deflection, ULONGLONG now) {
        const size_t index = static_cast<size_t>(key);
        const bool active = deflection >= (stickActive_[index] ? kStickRelease : kStickPress);
        if (pressed_[index] && now - tapStart_[index] >= kTapLength) Set(key, false);
        if (!active) {
            stickActive_[index] = false;
            return;
        }
        const bool first = !stickActive_[index];
        const ULONGLONG waited = now - lastTap_[index];
        if (first || (now - activeSince_[index] >= kFirstRepeat && waited >= kRepeatInterval)) {
            if (first) activeSince_[index] = now;
            stickActive_[index] = true;
            lastTap_[index] = now;
            tapStart_[index] = now;
            Set(key, true);
        }
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
    std::array<bool, kKeyCount> stickActive_{};
    std::array<ULONGLONG, kKeyCount> activeSince_{};
    std::array<ULONGLONG, kKeyCount> lastTap_{};
    std::array<ULONGLONG, kKeyCount> tapStart_{};
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
    float squareFovDegrees = 0.0f;
    // Vertical image scale tuned in the headset (both grips + right stick),
    // saved in TMFOXR-xrapi.txt beside the DLL. Above 1 the image gets taller.
    float verticalScale = 1.0f;
    bool verticalScaleLoaded = false;
    bool verticalScaleAdjusting = false;
    ULONGLONG lastScaleTick = 0;
    bool windowStereo = false;  // start flat: the game opens in its menus

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
    // Headset frames between latching a pose and presenting the frame.
    // WinlatorXR keeps only 22 poses (sync values 0..252 in steps of 12).
    uint64_t syncAgeTotal = 0;
    int syncAgeMax = 0;
    uint64_t syncAgeSamples = 0;
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
        // std::to_chars is locale independent; WinlatorXR parses with Java's Float.
        // MODE_VR 1 + MODE_3D 1: side-by-side VR. MODE_VR 2 + MODE_3D 0: the
        // window on a flat virtual screen without head tracking.
        std::string message = "0 0 0 0 0 0";
        if (enabled) {
            const auto format = [](float degrees) {
                char text[32]{};
                const auto end = std::to_chars(text, text + sizeof(text), degrees, std::chars_format::fixed, 2).ptr;
                return std::string(text, end);
            };
            message = std::string(windowStereo ? "0 0 1 1 " : "0 0 2 0 ") + format(HorizontalFovDegrees()) + " " +
                format(squareFovDegrees);
        }
        sendto(socket, message.c_str(), static_cast<int>(message.size()), 0,
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
                // In menus the left stick navigates with the arrow keys, so
                // the joypad reports it centred.
                ControllerState published = latest.controller;
                if (!windowStereo) published.leftStick[0] = published.leftStick[1] = 0.0f;
                PublishControllerState(published);
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

    // WinlatorXR's reported FOV and the FOV of its projection layer disagree on
    // which axis is which (the image appeared squeezed horizontally by ~0.8).
    // Requesting the same angle for both axes, as the Halo mod does, removes
    // the ambiguity; the larger reported angle keeps the whole view covered.
    void ChooseSquareFov(const TrackingSample& sample) {
        if (squareFovDegrees > 1.0f || sample.fovXDegrees <= 1.0f || sample.fovYDegrees <= 1.0f) return;
        squareFovDegrees = SquareFovOverride();
        if (squareFovDegrees <= 1.0f) squareFovDegrees = std::max(sample.fovXDegrees, sample.fovYDegrees);
        log::Info("XrAPI: headset reported FOV " + std::to_string(sample.fovXDegrees) + "x" +
            std::to_string(sample.fovYDegrees) + " degrees; requesting " + std::to_string(squareFovDegrees) +
            "x" + std::to_string(squareFovDegrees) + ".");
        SendMode(true);
    }

    // WinlatorXR shows each eye image with square pixels, so a non-square eye
    // image (960x1080 from a 1920x1080 screen) appeared squeezed. The vertical
    // angle is the chosen FOV; the horizontal one follows the eye image's
    // aspect ratio. A 2:1 screen gives square eyes and equal angles.
    float HorizontalFovDegrees() const {
        if (!eyeWidth || !eyeHeight || squareFovDegrees <= 1.0f) return squareFovDegrees;
        const float halfVertical = squareFovDegrees * kPi / 360.0f;
        const float halfHorizontal = std::atan(std::tan(halfVertical) * eyeWidth / eyeHeight);
        return halfHorizontal * 360.0f / kPi;
    }

    static std::filesystem::path VerticalScalePath() { return ModuleFilePath(L"TMFOXR-xrapi.txt"); }

    void LoadVerticalScale() {
        if (verticalScaleLoaded) return;
        verticalScaleLoaded = true;
        std::ifstream file(VerticalScalePath());
        float value = 0.0f;
        if (file >> value && value >= 0.5f && value <= 2.0f) verticalScale = value;
        log::Info("XrAPI: vertical image scale " + std::to_string(verticalScale) +
            " (hold both grips and move the right stick up/down to adjust).");
    }

    // Both grips held: the right stick scales the image vertically, live.
    void AdjustVerticalScale(const ControllerState& state) {
        const bool held = state.Pressed(kLeftGrip) && state.Pressed(kRightGrip);
        const float stick = state.rightStick[1];
        const ULONGLONG now = GetTickCount64();
        const float seconds = lastScaleTick ? std::min(0.1f, (now - lastScaleTick) / 1000.0f) : 0.0f;
        lastScaleTick = now;
        if (held && std::abs(stick) > 0.3f) {
            // About 10% per second at full deflection, independent of frame rate.
            // Content appears taller when the rendered vertical angle shrinks.
            verticalScale = std::clamp(verticalScale * std::pow(1.10f, stick * seconds), 0.5f, 2.0f);
            verticalScaleAdjusting = true;
            return;
        }
        if (!held && verticalScaleAdjusting) {
            verticalScaleAdjusting = false;
            std::ofstream(VerticalScalePath()) << verticalScale << "\n";
            log::Info("XrAPI: vertical image scale set to " + std::to_string(verticalScale) + " and saved.");
        }
    }

    void UpdateRenderConfiguration(const TrackingSample& sample) {
        LoadVerticalScale();
        const UINT previousEyeWidth = eyeWidth;
        const UINT previousEyeHeight = eyeHeight;
        UpdateEyeSize();
        ChooseSquareFov(sample);
        if (!eyeWidth || !eyeHeight || squareFovDegrees <= 1.0f) return;
        if (eyeWidth != previousEyeWidth || eyeHeight != previousEyeHeight) SendMode(true);
        const float halfX = HorizontalFovDegrees() * kPi / 360.0f;
        const float halfY = std::atan(std::tan(squareFovDegrees * kPi / 360.0f) / verticalScale);
        auto& eyes = renderConfiguration.eyes;
        if (haveRenderConfiguration && eyes[0].width == eyeWidth && eyes[0].height == eyeHeight &&
            eyes[0].angleRight == halfX && eyes[0].angleUp == halfY) return;
        const bool onlyScaleChanged = haveRenderConfiguration && eyes[0].width == eyeWidth && eyes[0].height == eyeHeight;
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
        if (onlyScaleChanged) return;
        log::Info("XrAPI: rendering " + std::to_string(eyeWidth) + "x" + std::to_string(eyeHeight) +
            " per eye with a " + std::to_string(HorizontalFovDegrees()) + "x" + std::to_string(squareFovDegrees) +
            " degree FOV (square pixels), IPD " + std::to_string(sample.ipd) + " m." +
            (eyeWidth == eyeHeight ? "" : " A 2:1 WinlatorXR screen size gives square eyes and the full horizontal FOV."));
    }

    void BeginFrame() {
        if (!initialized || frameLatched) return;
        Drain();
        if (windowStereo) WaitForNewSync();
        if (GetTickCount64() - lastModeSend >= kModeResendMilliseconds) SendMode(true);
        // Release held keys when WinlatorXR stops streaming (paused, closed).
        if (haveLatest && GetTickCount64() - lastSampleTick < kInputTimeoutMilliseconds) {
            keyMapper.Update(latest.controller, !windowStereo);
        } else {
            keyMapper.ReleaseAll();
            PublishControllerState({});
        }
        if (!haveLatest) return;
        frameLatched = true;
        frameSync = latest.sync;
        if (latest.ipd > 0.04f && latest.ipd < 0.09f) frameIpd = latest.ipd;
        AdjustVerticalScale(latest.controller);
        UpdateHeadPose(latest);
        UpdateRenderConfiguration(latest);
    }

    void EndFrame() {
        if (!initialized) return;
        if (presentedFrames == 300) LogWindowLayout();
        if (frameLatched && windowStereo) {
            Drain();
            constexpr int kSyncSlots = 22;
            const int age = ((latest.sync - frameSync) / 12 % kSyncSlots + kSyncSlots) % kSyncSlots;
            syncAgeTotal += static_cast<uint64_t>(age);
            syncAgeMax = std::max(syncAgeMax, age);
            if (++syncAgeSamples == 600) {
                log::Info("XrAPI: pose age at present over 600 VR frames: average " +
                    std::to_string(static_cast<double>(syncAgeTotal) / 600.0) + ", maximum " +
                    std::to_string(syncAgeMax) + " headset frames (WinlatorXR keeps " +
                    std::to_string(kSyncSlots) + ").");
                syncAgeTotal = 0;
                syncAgeMax = 0;
                syncAgeSamples = 0;
            }
        }
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
    // Also valid in flat mode: every frame carries a sync pixel, because
    // WinlatorXR learns its sync colour mapping from whatever pixel it reads
    // and a game-image pixel around a mode switch would corrupt it.
    if (!impl_->initialized || !impl_->frameLatched || impl_->frameSync < 0) return false;
    // Green must stay 0 and alpha non-zero; blue 0 selects the left/SBS target.
    presentation.syncColor = D3DCOLOR_ARGB(255, impl_->frameSync, 0, 0);
    return true;
}

void VrBridge::SetWindowStereo(bool stereo) {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->windowStereo == stereo) return;
    impl_->windowStereo = stereo;
    log::Info(stereo ? "XrAPI: race detected; switching WinlatorXR to side-by-side VR."
                     : "XrAPI: menu; switching WinlatorXR to its flat virtual screen.");
    if (impl_->initialized) impl_->SendMode(true);
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
