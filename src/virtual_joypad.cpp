// Virtual DirectInput 8 joypad fed by the Quest controllers (XrAPI builds).
//
// TrackMania reads game controllers through DirectInput 8. With the
// Competition Patch loaded, the game's DirectInput8Create import is bypassed,
// so dinput8.dll's export itself jumps to a wrapper. The wrapper lists one
// extra game controller, "Quest Controllers", and creates it on request, so
// the game can bind steering to the analog stick like any real pad.
//
// WinlatorXR keeps emulating a mouse with the right controller, so the
// wrapper also mutes the DirectInput system mouse; otherwise the right
// trigger would click an invisible pointer.

#define DIRECTINPUT_VERSION 0x0800
#include "controller_input.h"

#include "log.h"

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tmoxr {
namespace {
using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
DllGetClassObjectFn g_dinputClassObject = nullptr;

// {5155A8C1-7A2B-4E0E-9C3D-51A7F0E0D001}
constexpr GUID kJoypadInstanceGuid = {0x5155a8c1, 0x7a2b, 0x4e0e, {0x9c, 0x3d, 0x51, 0xa7, 0xf0, 0xe0, 0xd0, 0x01}};
// DirectInput product GUIDs carry the USB VID/PID in the first field.
constexpr WORD kVendorId = 0x2833;  // Oculus VR
constexpr WORD kProductId = 0x7154;
constexpr GUID kJoypadProductGuid = {
    static_cast<DWORD>(kProductId) << 16 | kVendorId, 0x0000, 0x0000,
    {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44}};  // "PIDVID"
constexpr wchar_t kJoypadName[] = L"Quest Controllers";
constexpr DWORD kJoypadDeviceType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);

enum class ObjectKind { Axis, Button };

struct JoypadObject {
    const GUID* type;
    ObjectKind kind;
    DWORD instance;
    DWORD nativeOffset;  // offset in DIJOYSTATE2
    const wchar_t* name;
    WORD usagePage;
    WORD usage;
};

// Button numbers as TrackMania shows them are the index + 1.
constexpr std::array<ControllerButton, 9> kJoypadButtons{
    kRightTrigger, kLeftTrigger, kButtonA, kButtonB, kButtonX, kButtonY,
    kRightGrip, kLeftGrip, kLeftThumbstickPress};
constexpr std::array<const wchar_t*, 9> kJoypadButtonNames{
    L"Right trigger", L"Left trigger", L"A", L"B", L"X", L"Y",
    L"Right grip", L"Left grip", L"Left stick press"};

std::vector<JoypadObject> BuildObjects() {
    std::vector<JoypadObject> objects{
        {&GUID_XAxis, ObjectKind::Axis, 0, DIJOFS_X, L"Left stick X", 0x01, 0x30},
        {&GUID_YAxis, ObjectKind::Axis, 1, DIJOFS_Y, L"Left stick Y", 0x01, 0x31},
        {&GUID_RxAxis, ObjectKind::Axis, 2, DIJOFS_RX, L"Right stick X", 0x01, 0x33},
        {&GUID_RyAxis, ObjectKind::Axis, 3, DIJOFS_RY, L"Right stick Y", 0x01, 0x34},
    };
    for (DWORD button = 0; button < kJoypadButtons.size(); ++button) {
        objects.push_back({&GUID_Button, ObjectKind::Button, button,
                           static_cast<DWORD>(DIJOFS_BUTTON(button)), kJoypadButtonNames[button],
                           0x09, static_cast<WORD>(button + 1)});
    }
    return objects;
}

const std::vector<JoypadObject>& Objects() {
    static const std::vector<JoypadObject> objects = BuildObjects();
    return objects;
}

DWORD ObjectTypeId(const JoypadObject& object) {
    return (object.kind == ObjectKind::Axis ? DIDFT_ABSAXIS : DIDFT_PSHBUTTON) |
           DIDFT_MAKEINSTANCE(object.instance);
}

// Raw value of an object: axes -1..1 (DirectInput Y grows downward), buttons 0/1.
float ObjectValue(const JoypadObject& object, const ControllerState& state) {
    if (!state.connected) return 0.0f;
    if (object.kind == ObjectKind::Button) {
        return state.Pressed(kJoypadButtons[object.instance]) ? 1.0f : 0.0f;
    }
    switch (object.instance) {
    case 0: return state.leftStick[0];
    case 1: return -state.leftStick[1];
    case 2: return state.rightStick[0];
    default: return -state.rightStick[1];
    }
}

// DirectInput property "GUIDs" are small integers cast to GUID pointers
// (MAKEDIPROP). Compare the integers; comparing two such references is
// undefined and the compiler folds it to false.
enum PropertyId : uintptr_t {
    kPropBufferSize = 1, kPropAxisMode = 2, kPropGranularity = 3, kPropRange = 4,
    kPropDeadZone = 5, kPropSaturation = 6, kPropFfGain = 7, kPropAutoCenter = 9,
    kPropCalibrationMode = 10, kPropInstanceName = 13, kPropProductName = 14,
    kPropJoystickId = 15, kPropVidPid = 24,
};

[[gnu::noinline]] uintptr_t PropertyIdOf(REFGUID property) {
    return reinterpret_cast<uintptr_t>(&property);
}

bool IsProperty(REFGUID property, PropertyId expected) {
    return PropertyIdOf(property) == expected;
}

class VirtualJoypad final : public IDirectInputDevice8W {
public:
    VirtualJoypad() {
        axes_.fill(AxisSettings{});
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDirectInputDevice8W) {
            *object = static_cast<IDirectInputDevice8W*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return remaining;
    }

    // IDirectInputDevice8W
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
        if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3)) return DIERR_INVALIDPARAM;
        const DWORD size = caps->dwSize;
        std::memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwFlags = DIDC_ATTACHED;
        caps->dwDevType = kJoypadDeviceType;
        caps->dwAxes = 4;
        caps->dwButtons = static_cast<DWORD>(kJoypadButtons.size());
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW callback, LPVOID context,
                                          DWORD flags) override {
        if (!callback) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        for (const auto& object : Objects()) {
            const DWORD type = DIDFT_GETTYPE(flags);
            if (type && !(type & (object.kind == ObjectKind::Axis ? DIDFT_AXIS : DIDFT_BUTTON))) continue;
            DIDEVICEOBJECTINSTANCEW instance{};
            FillObjectInstance(object, instance);
            if (callback(&instance, context) == DIENUM_STOP) break;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER header) override {
        if (!header) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        if (IsProperty(property, kPropBufferSize)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = bufferSize_;
            return DI_OK;
        }
        if (IsProperty(property, kPropAxisMode)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = DIPROPAXISMODE_ABS;
            return DI_OK;
        }
        if (IsProperty(property, kPropJoystickId)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = 0;
            return DI_OK;
        }
        if (IsProperty(property, kPropVidPid)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = MAKELONG(kVendorId, kProductId);
            return DI_OK;
        }
        if (IsProperty(property, kPropProductName) || IsProperty(property, kPropInstanceName)) {
            auto* text = reinterpret_cast<DIPROPSTRING*>(header);
            lstrcpynW(text->wsz, kJoypadName, MAX_PATH);
            return DI_OK;
        }
        const int axis = AxisForProperty(*header);
        if (axis < 0) return DIERR_UNSUPPORTED;
        const AxisSettings& settings = axes_[axis];
        if (IsProperty(property, kPropRange)) {
            auto* range = reinterpret_cast<DIPROPRANGE*>(header);
            range->lMin = settings.minimum;
            range->lMax = settings.maximum;
            return DI_OK;
        }
        if (IsProperty(property, kPropDeadZone)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = settings.deadZone;
            return DI_OK;
        }
        if (IsProperty(property, kPropSaturation)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = settings.saturation;
            return DI_OK;
        }
        if (IsProperty(property, kPropGranularity)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = 1;
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER header) override {
        if (!header) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        if (IsProperty(property, kPropBufferSize)) {
            bufferSize_ = reinterpret_cast<const DIPROPDWORD*>(header)->dwData;
            events_.clear();
            return DI_OK;
        }
        if (IsProperty(property, kPropAxisMode) || IsProperty(property, kPropAutoCenter) ||
            IsProperty(property, kPropFfGain) || IsProperty(property, kPropCalibrationMode)) {
            return DI_OK;
        }
        const bool range = IsProperty(property, kPropRange);
        const bool deadZone = IsProperty(property, kPropDeadZone);
        const bool saturation = IsProperty(property, kPropSaturation);
        if (!range && !deadZone && !saturation) return DIERR_UNSUPPORTED;
        const auto apply = [&](AxisSettings& settings) {
            if (range) {
                const auto* value = reinterpret_cast<const DIPROPRANGE*>(header);
                settings.minimum = value->lMin;
                settings.maximum = value->lMax;
            } else if (deadZone) {
                settings.deadZone = std::min<DWORD>(reinterpret_cast<const DIPROPDWORD*>(header)->dwData, 10000);
            } else {
                settings.saturation = std::min<DWORD>(reinterpret_cast<const DIPROPDWORD*>(header)->dwData, 10000);
            }
        };
        if (header->dwHow == DIPH_DEVICE) {
            for (auto& settings : axes_) apply(settings);
            return DI_OK;
        }
        const int axis = AxisForProperty(*header);
        if (axis < 0) return DIERR_OBJECTNOTFOUND;
        apply(axes_[axis]);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Acquire() override {
        std::lock_guard lock(mutex_);
        if (acquired_) return S_FALSE;
        acquired_ = true;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Unacquire() override {
        std::lock_guard lock(mutex_);
        if (!acquired_) return DI_NOEFFECT;
        acquired_ = false;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override {
        std::lock_guard lock(mutex_);
        if (!data || size != dataSize_) return DIERR_INVALIDPARAM;
        if (!acquired_) return DIERR_NOTACQUIRED;
        Update();
        auto* bytes = static_cast<uint8_t*>(data);
        std::memset(bytes, 0, size);
        for (const auto& field : fields_) {
            if (field.object < 0) {
                if (field.pov && field.offset + sizeof(DWORD) <= size) {
                    const DWORD centered = 0xFFFFFFFFu;
                    std::memcpy(bytes + field.offset, &centered, sizeof(centered));
                }
                continue;
            }
            WriteValue(bytes, size, field, current_[field.object]);
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD objectSize, LPDIDEVICEOBJECTDATA records, LPDWORD count,
                                            DWORD flags) override {
        if (!count) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        if (!acquired_) return DIERR_NOTACQUIRED;
        if (!bufferSize_) return DIERR_NOTBUFFERED;
        if (objectSize != sizeof(DIDEVICEOBJECTDATA) && objectSize != sizeof(DIDEVICEOBJECTDATA_DX3)) {
            return DIERR_INVALIDPARAM;
        }
        Update();
        const DWORD available = static_cast<DWORD>(events_.size());
        const DWORD wanted = records ? std::min(*count, available) : std::min(*count, available);
        if (records) {
            auto* output = reinterpret_cast<uint8_t*>(records);
            for (DWORD index = 0; index < wanted; ++index) {
                DIDEVICEOBJECTDATA record{};
                const Event& event = events_[index];
                record.dwOfs = event.offset;
                record.dwData = event.value;
                record.dwTimeStamp = event.timestamp;
                record.dwSequence = event.sequence;
                std::memcpy(output + index * objectSize, &record, objectSize);
            }
        }
        *count = wanted;
        if (!(flags & DIGDD_PEEK)) events_.erase(events_.begin(), events_.begin() + wanted);
        const bool overflowed = overflowed_;
        if (!(flags & DIGDD_PEEK)) overflowed_ = false;
        return overflowed ? DI_BUFFEROVERFLOW : DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override {
        if (!format || !format->rgodf || format->dwObjSize != sizeof(DIOBJECTDATAFORMAT)) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        if (acquired_) return DIERR_ACQUIRED;
        std::vector<bool> used(Objects().size(), false);
        fields_.clear();
        for (DWORD index = 0; index < format->dwNumObjs; ++index) {
            const DIOBJECTDATAFORMAT& entry = format->rgodf[index];
            Field field{entry.dwOfs, -1, (DIDFT_GETTYPE(entry.dwType) & DIDFT_POV) != 0,
                        (DIDFT_GETTYPE(entry.dwType) & DIDFT_BUTTON) != 0};
            const DWORD instance = DIDFT_GETINSTANCE(entry.dwType);
            for (size_t object = 0; object < Objects().size(); ++object) {
                const JoypadObject& candidate = Objects()[object];
                if (used[object]) continue;
                if (entry.pguid && *entry.pguid != *candidate.type) continue;
                const DWORD wantedType = DIDFT_GETTYPE(entry.dwType);
                const bool kindMatches = candidate.kind == ObjectKind::Axis
                    ? (wantedType & DIDFT_AXIS) != 0 : (wantedType & DIDFT_BUTTON) != 0;
                if (!kindMatches) continue;
                if (instance != 0xFFFF && instance != candidate.instance) continue;
                field.object = static_cast<int>(object);
                used[object] = true;
                break;
            }
            fields_.push_back(field);
        }
        dataSize_ = format->dwDataSize;
        events_.clear();
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) override {
        std::lock_guard lock(mutex_);
        notification_ = event;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW info, DWORD objectId, DWORD how) override {
        if (!info) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        const int object = FindObject(objectId, how);
        if (object < 0) return DIERR_OBJECTNOTFOUND;
        FillObjectInstance(Objects()[object], *info);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEW info) override {
        if (!info || info->dwSize < sizeof(DIDEVICEINSTANCE_DX3W)) return DIERR_INVALIDPARAM;
        FillDeviceInstance(*info);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT* effect, LPUNKNOWN) override {
        if (effect) *effect = nullptr;
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKW, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOW, REFGUID) override { return DIERR_DEVICENOTREG; }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override {
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Poll() override {
        std::lock_guard lock(mutex_);
        if (!acquired_) return DIERR_NOTACQUIRED;
        Update();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override {
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCWSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override {
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCWSTR, DWORD, LPDIFILEEFFECT, DWORD) override {
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATW, LPCWSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATW, LPCWSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW) override { return DIERR_UNSUPPORTED; }

    static void FillDeviceInstance(DIDEVICEINSTANCEW& info) {
        const DWORD size = info.dwSize;
        std::memset(&info, 0, size);
        info.dwSize = size;
        info.guidInstance = kJoypadInstanceGuid;
        info.guidProduct = kJoypadProductGuid;
        info.dwDevType = kJoypadDeviceType;
        lstrcpynW(info.tszInstanceName, kJoypadName, MAX_PATH);
        lstrcpynW(info.tszProductName, kJoypadName, MAX_PATH);
        if (size >= sizeof(DIDEVICEINSTANCEW)) {
            info.wUsagePage = 0x01;
            info.wUsage = 0x05;  // game pad
        }
    }

private:
    struct AxisSettings {
        LONG minimum = 0;
        LONG maximum = 65535;
        DWORD deadZone = 0;
        DWORD saturation = 10000;
    };
    struct Field {
        DWORD offset;
        int object;  // index into Objects(), or -1 when the app asked for something we lack
        bool pov;
        bool button;
    };
    struct Event {
        DWORD offset;
        DWORD value;
        DWORD timestamp;
        DWORD sequence;
    };

    int AxisForProperty(const DIPROPHEADER& header) const {
        const int object = FindObject(header.dwObj, header.dwHow);
        if (object < 0 || Objects()[object].kind != ObjectKind::Axis) return -1;
        return static_cast<int>(Objects()[object].instance);
    }

    int FindObject(DWORD objectId, DWORD how) const {
        if (how == DIPH_BYOFFSET) {
            for (const auto& field : fields_) {
                if (field.offset == objectId) return field.object;
            }
            return -1;
        }
        if (how == DIPH_BYID) {
            for (size_t object = 0; object < Objects().size(); ++object) {
                if (ObjectTypeId(Objects()[object]) == objectId) return static_cast<int>(object);
            }
            return -1;
        }
        if (how == DIPH_BYUSAGE) {
            for (size_t object = 0; object < Objects().size(); ++object) {
                if (MAKELONG(Objects()[object].usage, Objects()[object].usagePage) == static_cast<LONG>(objectId)) {
                    return static_cast<int>(object);
                }
            }
        }
        return -1;
    }

    DWORD DataOffset(size_t object) const {
        for (const auto& field : fields_) {
            if (field.object == static_cast<int>(object)) return field.offset;
        }
        return Objects()[object].nativeOffset;
    }

    void FillObjectInstance(const JoypadObject& object, DIDEVICEOBJECTINSTANCEW& info) const {
        const DWORD size = info.dwSize >= sizeof(DIDEVICEOBJECTINSTANCE_DX3W) ? info.dwSize : sizeof(info);
        std::memset(&info, 0, size);
        info.dwSize = size;
        info.guidType = *object.type;
        info.dwOfs = DataOffset(static_cast<size_t>(&object - Objects().data()));
        info.dwType = ObjectTypeId(object);
        info.dwFlags = object.kind == ObjectKind::Axis ? DIDOI_ASPECTPOSITION : 0;
        lstrcpynW(info.tszName, object.name, MAX_PATH);
        if (size >= sizeof(DIDEVICEOBJECTINSTANCEW)) {
            info.wUsagePage = object.usagePage;
            info.wUsage = object.usage;
        }
    }

    // Scales an axis to the application's range with dead zone and saturation.
    LONG AxisValue(int axis, float value) const {
        const AxisSettings& settings = axes_[axis];
        const float magnitude = std::fabs(value);
        const float deadZone = settings.deadZone / 10000.0f;
        const float saturation = std::max(settings.saturation / 10000.0f, deadZone + 0.001f);
        float scaled = magnitude <= deadZone ? 0.0f
            : std::min(1.0f, (magnitude - deadZone) / (saturation - deadZone));
        scaled = std::copysign(scaled, value);
        const double center = (static_cast<double>(settings.minimum) + settings.maximum) / 2.0;
        const double halfRange = (static_cast<double>(settings.maximum) - settings.minimum) / 2.0;
        return static_cast<LONG>(std::lround(center + scaled * halfRange));
    }

    DWORD EncodedValue(size_t object, float value) const {
        const JoypadObject& info = Objects()[object];
        if (info.kind == ObjectKind::Button) return value > 0.5f ? 0x80u : 0u;
        return static_cast<DWORD>(AxisValue(static_cast<int>(info.instance), value));
    }

    void WriteValue(uint8_t* bytes, DWORD size, const Field& field, float value) const {
        const DWORD encoded = EncodedValue(static_cast<size_t>(field.object), value);
        if (field.button) {
            if (field.offset < size) bytes[field.offset] = static_cast<uint8_t>(encoded);
        } else if (field.offset + sizeof(LONG) <= size) {
            std::memcpy(bytes + field.offset, &encoded, sizeof(encoded));
        }
    }

    void Update() {
        const ControllerState state = GetControllerState();
        const DWORD now = GetTickCount();
        bool changed = false;
        for (size_t object = 0; object < Objects().size(); ++object) {
            const float value = ObjectValue(Objects()[object], state);
            const DWORD previous = EncodedValue(object, current_[object]);
            current_[object] = value;
            const DWORD encoded = EncodedValue(object, value);
            if (encoded == previous || !bufferSize_) continue;
            changed = true;
            // Buffered data only reports objects that are part of the data format.
            bool inFormat = false;
            for (const auto& field : fields_) inFormat |= field.object == static_cast<int>(object);
            if (!inFormat) continue;
            if (events_.size() >= bufferSize_) {
                events_.pop_front();
                overflowed_ = true;
            }
            events_.push_back({DataOffset(object), encoded, now, ++sequence_});
        }
        if (changed && notification_) SetEvent(notification_);
        if (changed && !loggedFirstInput_) {
            loggedFirstInput_ = true;
            log::Info("Virtual joypad: first Quest controller input reached TrackMania.");
        }
    }

    volatile LONG references_ = 1;
    // Recursive: the game calls SetProperty from inside its EnumObjects callback.
    std::recursive_mutex mutex_;
    std::array<AxisSettings, 4> axes_{};
    std::vector<Field> fields_;
    DWORD dataSize_ = 0;
    bool acquired_ = false;
    DWORD bufferSize_ = 0;
    std::deque<Event> events_;
    bool overflowed_ = false;
    DWORD sequence_ = 0;
    HANDLE notification_ = nullptr;
    std::array<float, 13> current_{};
    bool loggedFirstInput_ = false;
};

// Passes the system mouse through but reports no motion or buttons.
class MutedMouse final : public IDirectInputDevice8W {
public:
    explicit MutedMouse(IDirectInputDevice8W* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDirectInputDevice8W) {
            *object = static_cast<IDirectInputDevice8W*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(id, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = InterlockedDecrement(&references_);
        if (!remaining) {
            real_->Release();
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override { return real_->GetCapabilities(caps); }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW callback, LPVOID context, DWORD flags) override {
        return real_->EnumObjects(callback, context, flags);
    }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER header) override {
        return real_->GetProperty(property, header);
    }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER header) override {
        return real_->SetProperty(property, header);
    }
    HRESULT STDMETHODCALLTYPE Acquire() override { return real_->Acquire(); }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return real_->Unacquire(); }
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override {
        const HRESULT result = real_->GetDeviceState(size, data);
        if (SUCCEEDED(result) && data) std::memset(data, 0, size);
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD objectSize, LPDIDEVICEOBJECTDATA records, LPDWORD count,
                                            DWORD flags) override {
        // Drain the real buffer so it does not overflow, then report nothing.
        const HRESULT result = real_->GetDeviceData(objectSize, records, count, flags);
        if (SUCCEEDED(result) && count) *count = 0;
        return SUCCEEDED(result) ? DI_OK : result;
    }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override { return real_->SetDataFormat(format); }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) override { return real_->SetEventNotification(event); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override {
        return real_->SetCooperativeLevel(window, flags);
    }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW info, DWORD object, DWORD how) override {
        return real_->GetObjectInfo(info, object, how);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEW info) override { return real_->GetDeviceInfo(info); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override {
        return real_->RunControlPanel(window, flags);
    }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version, REFGUID guid) override {
        return real_->Initialize(instance, version, guid);
    }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID guid, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out,
                                           LPUNKNOWN outer) override {
        return real_->CreateEffect(guid, effect, out, outer);
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKW callback, LPVOID context, DWORD type) override {
        return real_->EnumEffects(callback, context, type);
    }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOW info, REFGUID guid) override {
        return real_->GetEffectInfo(info, guid);
    }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD state) override {
        return real_->GetForceFeedbackState(state);
    }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD command) override {
        return real_->SendForceFeedbackCommand(command);
    }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback,
                                                       LPVOID context, DWORD flags) override {
        return real_->EnumCreatedEffectObjects(callback, context, flags);
    }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) override { return real_->Escape(escape); }
    HRESULT STDMETHODCALLTYPE Poll() override { return real_->Poll(); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD count,
                                             DWORD flags) override {
        return real_->SendDeviceData(size, data, count, flags);
    }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCWSTR file, LPDIENUMEFFECTSINFILECALLBACK callback,
                                                LPVOID context, DWORD flags) override {
        return real_->EnumEffectsInFile(file, callback, context, flags);
    }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCWSTR file, DWORD count, LPDIFILEEFFECT effects,
                                                DWORD flags) override {
        return real_->WriteEffectToFile(file, count, effects, flags);
    }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATW format, LPCWSTR user, DWORD flags) override {
        return real_->BuildActionMap(format, user, flags);
    }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATW format, LPCWSTR user, DWORD flags) override {
        return real_->SetActionMap(format, user, flags);
    }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW header) override {
        return real_->GetImageInfo(header);
    }

private:
    IDirectInputDevice8W* real_;
    volatile LONG references_ = 1;
};

bool EnumeratesGameControllers(DWORD deviceType) {
    return deviceType == DI8DEVCLASS_ALL || deviceType == DI8DEVCLASS_GAMECTRL ||
           deviceType == DI8DEVTYPE_GAMEPAD || deviceType == DI8DEVTYPE_JOYSTICK;
}

class DirectInputWithJoypad final : public IDirectInput8W {
public:
    explicit DirectInputWithJoypad(IDirectInput8W* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDirectInput8W) {
            *object = static_cast<IDirectInput8W*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(id, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = InterlockedDecrement(&references_);
        if (!remaining) {
            real_->Release();
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8W* device, LPUNKNOWN outer) override {
        if (!device) return DIERR_INVALIDPARAM;
        if (guid == kJoypadInstanceGuid || guid == kJoypadProductGuid) {
            *device = new VirtualJoypad();
            log::Info("Created the virtual \"Quest Controllers\" joypad for TrackMania.");
            return DI_OK;
        }
        const HRESULT result = real_->CreateDevice(guid, device, outer);
        if (SUCCEEDED(result) && *device && guid == GUID_SysMouse) {
            *device = new MutedMouse(*device);
            log::Info("Muted the DirectInput system mouse; WinlatorXR's pointer emulation does not reach the game.");
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD deviceType, LPDIENUMDEVICESCALLBACKW callback, LPVOID context,
                                          DWORD flags) override {
        if (!callback) return DIERR_INVALIDPARAM;
        // List the Quest pad first so games that pick the first pad use it.
        if (EnumeratesGameControllers(deviceType) && !(flags & DIEDFL_FORCEFEEDBACK)) {
            DIDEVICEINSTANCEW instance{};
            instance.dwSize = sizeof(instance);
            VirtualJoypad::FillDeviceInstance(instance);
            if (callback(&instance, context) == DIENUM_STOP) return DI_OK;
        }
        return real_->EnumDevices(deviceType, callback, context, flags);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override {
        if (guid == kJoypadInstanceGuid || guid == kJoypadProductGuid) return DI_OK;
        return real_->GetDeviceStatus(guid);
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override {
        return real_->RunControlPanel(window, flags);
    }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) override {
        return real_->Initialize(instance, version);
    }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID guid, LPCWSTR name, LPGUID instance) override {
        return real_->FindDevice(guid, name, instance);
    }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR user, LPDIACTIONFORMATW format,
                                                     LPDIENUMDEVICESBYSEMANTICSCBW callback, LPVOID context,
                                                     DWORD flags) override {
        return real_->EnumDevicesBySemantics(user, format, callback, context, flags);
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK callback, LPDICONFIGUREDEVICESPARAMSW params,
                                               DWORD flags, LPVOID data) override {
        return real_->ConfigureDevices(callback, params, flags, data);
    }

private:
    IDirectInput8W* real_;
    volatile LONG references_ = 1;
};

// Replaces dinput8!DirectInput8Create. The real object comes from the DLL's
// class factory, so the patched export never has to run its original code.
HRESULT WINAPI DirectInput8CreateHook(HINSTANCE instance, DWORD version, REFIID id, LPVOID* out, LPUNKNOWN outer) {
    if (!out) return DIERR_INVALIDPARAM;
    *out = nullptr;
    IClassFactory* factory = nullptr;
    HRESULT result = g_dinputClassObject(CLSID_DirectInput8, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(result)) return result;
    result = factory->CreateInstance(outer, id, out);
    factory->Release();
    if (FAILED(result) || !*out) return result;
    // TrackMania uses the Unicode interfaces; ANSI callers get plain DirectInput.
    if (id == IID_IDirectInput8W) {
        auto* input = static_cast<IDirectInput8W*>(*out);
        result = input->Initialize(instance, version);
        if (SUCCEEDED(result) && !outer) *out = static_cast<IDirectInput8W*>(new DirectInputWithJoypad(input));
    } else if (id == IID_IDirectInput8A) {
        result = static_cast<IDirectInput8A*>(*out)->Initialize(instance, version);
    }
    if (FAILED(result)) {
        static_cast<IUnknown*>(*out)->Release();
        *out = nullptr;
    }
    return result;
}
} // namespace

void InstallVirtualJoypad() {
    const HMODULE dinput = LoadLibraryW(L"dinput8.dll");
    auto* const target = dinput ? reinterpret_cast<uint8_t*>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
    g_dinputClassObject = dinput
        ? reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dinput, "DllGetClassObject")) : nullptr;
    if (!target || !g_dinputClassObject) {
        log::Warn("dinput8.dll lacks DirectInput8Create or DllGetClassObject; the virtual joypad is unavailable.");
        return;
    }
    const intptr_t displacement = reinterpret_cast<intptr_t>(&DirectInput8CreateHook) -
                                  reinterpret_cast<intptr_t>(target + 5);
    DWORD protection = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &protection)) {
        log::Warn("Could not patch dinput8!DirectInput8Create; the virtual joypad is unavailable.");
        return;
    }
    target[0] = 0xE9;  // jmp rel32
    const int32_t relative = static_cast<int32_t>(displacement);
    std::memcpy(target + 1, &relative, sizeof(relative));
    DWORD ignored = 0;
    VirtualProtect(target, 5, protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    log::Info("Installed the virtual Quest Controllers joypad (dinput8!DirectInput8Create).");
}
} // namespace tmoxr
