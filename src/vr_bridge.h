#pragma once

#include <d3d9.h>
#include <cstdint>

namespace tmoxr {
struct HeadPose {
    float position[3]{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    // Measured eye separation in metres; 0 when the backend does not report it.
    float ipd = 0.0f;
    uint64_t sample = 0;
};

struct EyeRenderConfiguration {
    uint32_t width = 0;
    uint32_t height = 0;
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleDown = 0.0f;
    float angleUp = 0.0f;
};

struct RenderConfiguration {
    EyeRenderConfiguration eyes[2]{};
    uint64_t sample = 0;
};

// Backends such as WinlatorXR's XrAPI display the game window itself as a
// side-by-side stereo frame instead of receiving eye textures.
struct WindowPresentation {
    // Written to the frame's top-left pixel so the host can pair the frame
    // with the head pose it was rendered from.
    D3DCOLOR syncColor = 0;
};

class VrBridge {
public:
    static VrBridge& Instance();
    // True when the backend shows the game's own window in the headset, so the
    // window may be fullscreen or larger than the desktop work area.
    static bool UsesGameWindowAsDisplay();
    bool GetWindowPresentation(WindowPresentation& presentation);
    // Window-display backends only: true renders the game in stereo 3D,
    // false shows it on a curved virtual screen inside VR (menus).
    void SetWindowStereo(bool stereo);
    // Window-display backends, menus only: switches the frame to the newest
    // head pose just before the menu screen is drawn, since the menu image
    // itself does not depend on the pose. Returns true when the pose changed.
    bool RelatchPoseForMenu();
    void OnDeviceCreated(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& parameters);
    bool TryInitialize();
    void OnBeginScene();
    void OnBeforePresent(IDirect3DDevice9* device);
    void OnBeforeReset();
    void OnTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX& matrix);
    void OnGameProjection(const D3DMATRIX& matrix);
    void OnRenderTarget(IDirect3DSurface9* surface);
    void OnDraw(bool indexed);
    void SetLeftEyeSurface(IDirect3DSurface9* surface, HANDLE sharedHandle = nullptr,
                           uint32_t sourceX = 0);
    void SetRightEyeSurface(IDirect3DSurface9* surface, HANDLE sharedHandle = nullptr,
                            uint32_t sourceX = 0);
    void SetUiSurface(IDirect3DSurface9* surface, HANDLE sharedHandle = nullptr);
    bool GetHeadPose(HeadPose& pose);
    bool GetRenderConfiguration(RenderConfiguration& configuration);
    void SetRecenterOnTrackingJump(bool enabled);
    void SetVerboseDiagnostics(bool enabled);
    void Shutdown();

private:
    VrBridge() = default;
    ~VrBridge();
    VrBridge(const VrBridge&) = delete;
    VrBridge& operator=(const VrBridge&) = delete;
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace tmoxr
