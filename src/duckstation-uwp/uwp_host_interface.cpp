#include "uwp_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/controller_interface.h"
#include "frontend-common/d3d11_host_display.h"
#include "frontend-common/d3d12_host_display.h"
#include "frontend-common/fullscreen_ui.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "uwp_key_map.h"
#include <cinttypes>
#include <cmath>
Log_SetChannel(UWPHostInterface);

#include <gamingdeviceinformation.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>

static bool IsRunningOnXbox()
{
  const auto version_info = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo();
  const auto device_family = version_info.DeviceFamily();
  return (device_family != L"Windows.Xbox");
}

UWPHostInterface::UWPHostInterface() = default;

UWPHostInterface::~UWPHostInterface() = default;

winrt::Windows::ApplicationModel::Core::IFrameworkView UWPHostInterface::CreateView()
{
  return *this;
}

void UWPHostInterface::Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView& a) {}

void UWPHostInterface::Load(const winrt::hstring&) {}

void UWPHostInterface::Uninitialize() {}

const char* UWPHostInterface::GetFrontendName() const
{
  return "DuckStation UWP Frontend";
}

bool UWPHostInterface::Initialize()
{
  Log::SetDebugOutputParams(true, nullptr, LOGLEVEL_DEBUG);
  if (!SetDirectories())
    return false;

  m_settings_interface = std::make_unique<INISettingsInterface>(GetSettingsFileName());
  m_flags.force_fullscreen_ui = true;
  m_fullscreen_ui_enabled = true;

  if (!CommonHostInterface::Initialize())
    return false;

  SetImGuiKeyMap();

  const bool start_fullscreen = m_flags.start_fullscreen || g_settings.start_fullscreen;
  if (!CreateDisplay(start_fullscreen))
  {
    Log_ErrorPrintf("Failed to create host display");
    return false;
  }

  return true;
}

void UWPHostInterface::Shutdown()
{
  DestroyDisplay();

  CommonHostInterface::Shutdown();
}

bool UWPHostInterface::CreateDisplay(bool fullscreen)
{
  Assert(!m_display);

  m_appview = winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
  m_appview.PreferredLaunchWindowingMode(
    fullscreen ? winrt::Windows::UI::ViewManagement::ApplicationViewWindowingMode::FullScreen :
                 winrt::Windows::UI::ViewManagement::ApplicationViewWindowingMode::Auto);

  m_window.Activate();

  const auto di = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
  const auto hdi = winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
  const s32 resolution_scale = static_cast<s32>(di.ResolutionScale());

  WindowInfo wi;
  wi.type = WindowInfo::Type::WinRT;
  wi.window_handle = winrt::get_unknown(m_window);
  wi.surface_scale = static_cast<float>(resolution_scale) / 100.0f;
  wi.surface_width = static_cast<u32>(m_window.Bounds().Width * wi.surface_scale);
  wi.surface_height = static_cast<s32>(m_window.Bounds().Height * wi.surface_scale);
  if (hdi)
  {
    const auto dm = hdi.GetCurrentDisplayMode();
    Log_InfoPrintf("HDMI mode: %ux%u @ %.2f hz", dm.ResolutionWidthInRawPixels(), dm.ResolutionHeightInRawPixels(),
                   dm.RefreshRate());

    wi.surface_refresh_rate = static_cast<float>(dm.RefreshRate());

    // Check for Xbox Series consoles, where we can create a 4K swap chain.
    if (IsRunningOnXbox())
    {
      GAMING_DEVICE_MODEL_INFORMATION gdinfo = {};
      if (SUCCEEDED(GetGamingDeviceModelInformation(&gdinfo)) && gdinfo.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT)
      {
        if (gdinfo.deviceId != GAMING_DEVICE_DEVICE_ID_XBOX_ONE &&
            gdinfo.deviceId != GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S)
        {
          // Are we running in 4K?
          if (dm.ResolutionWidthInRawPixels() >= 3840 && dm.ResolutionHeightInRawPixels() >= 2160)
          {
            Log_InfoPrintf("Setting up 4K swap chain for Xbox Series console");
            wi.surface_scale *= 3840.0f / static_cast<float>(wi.surface_width);
            wi.surface_width = 3840;
            wi.surface_height = 2160;
          }
        }
      }
    }
  }

  if (g_settings.gpu_renderer == GPURenderer::HardwareD3D12)
    m_display = std::make_unique<FrontendCommon::D3D12HostDisplay>();
  else
    m_display = std::make_unique<FrontendCommon::D3D11HostDisplay>();

  if (!m_display->CreateRenderDevice(wi, g_settings.gpu_adapter, g_settings.gpu_use_debug_device,
                                     g_settings.gpu_threaded_presentation) ||
      !m_display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device,
                                         g_settings.gpu_threaded_presentation) ||
      !CreateHostDisplayResources())
  {
    m_display->DestroyRenderDevice();
    m_display.reset();
    ReportError("Failed to create/initialize display render device");
    return false;
  }

  if (!CreateHostDisplayResources())
    Log_WarningPrint("Failed to create host display resources");

  Log_InfoPrintf("Host display initialized at %ux%u resolution", m_display->GetWindowWidth(),
                 m_display->GetWindowHeight());
  return true;
}

void UWPHostInterface::DestroyDisplay()
{
  ReleaseHostDisplayResources();

  if (m_display)
    m_display->DestroyRenderDevice();

  m_display.reset();
}

bool UWPHostInterface::AcquireHostDisplay()
{
  return true;
}

void UWPHostInterface::ReleaseHostDisplay()
{
  // restore vsync, since we don't want to burn cycles at the menu
  m_display->SetVSync(true);
}

void UWPHostInterface::PollAndUpdate()
{
  CommonHostInterface::PollAndUpdate();

  ImGuiIO& io = ImGui::GetIO();
  if (m_text_input_requested != io.WantTextInput)
  {
    const bool activate = io.WantTextInput;
    Log_InfoPrintf("%s input pane...", activate ? "showing" : "hiding");

    m_text_input_requested = activate;
    m_dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [this, activate]() {
      const auto input_pane = winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView();
      if (input_pane)
      {
        if (activate)
          input_pane.TryShow();
        else
          input_pane.TryHide();
      }
    });
  }
}

void UWPHostInterface::RequestExit()
{
  m_shutdown_flag.store(true);
  m_dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [this]() { winrt::Windows::ApplicationModel::Core::CoreApplication::Exit(); });
}

void UWPHostInterface::Run()
{
  if (!Initialize())
  {
    Shutdown();
    return;
  }

  m_emulation_thread = std::thread(&UWPHostInterface::EmulationThreadEntryPoint, this);

  m_dispatcher.ProcessEvents(winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessUntilQuit);
  m_shutdown_flag.store(true);
  m_emulation_thread.join();
}

void UWPHostInterface::EmulationThreadEntryPoint()
{
  if (m_fullscreen_ui_enabled)
  {
    FullscreenUI::SetDebugMenuAllowed(true);
    FullscreenUI::QueueGameListRefresh();
  }

  // process events to pick up controllers before updating input map
  PollAndUpdate();
  UpdateInputMap();

  while (!m_shutdown_flag.load())
  {
    RunCallbacks();
    PollAndUpdate();

    ImGui::NewFrame();

    if (System::IsRunning())
    {
      if (m_display_all_frames)
        System::RunFrame();
      else
        System::RunFrames();

      UpdateControllerMetaState();
      if (m_frame_step_request)
      {
        m_frame_step_request = false;
        PauseSystem(true);
      }
    }

    // rendering
    {
      DrawImGuiWindows();
      ImGui::Render();
      ImGui::EndFrame();

      m_display->Render();

      if (System::IsRunning())
      {
        System::UpdatePerformanceCounters();

        if (m_throttler_enabled)
          System::Throttle();
      }
    }
  }

  // Save state on exit so it can be resumed
  if (!System::IsShutdown())
    PowerOffSystem(ShouldSaveResumeState());
}

void UWPHostInterface::ReportMessage(const char* message)
{
  Log_InfoPrint(message);
  AddOSDMessage(message, 10.0f);
}

void UWPHostInterface::ReportError(const char* message)
{
  Log_ErrorPrint(message);

  if (!m_display)
    return;

  const bool was_in_frame = GImGui->FrameCount != GImGui->FrameCountEnded;
  if (was_in_frame)
    ImGui::EndFrame();

  bool done = false;
  while (!done)
  {
    RunCallbacks();
    PollAndUpdate();
    if (m_fullscreen_ui_enabled)
      FullscreenUI::SetImGuiNavInputs();

    ImGui::NewFrame();
    done = FullscreenUI::DrawErrorWindow(message);
    ImGui::EndFrame();
    m_display->Render();
  }

  if (was_in_frame)
    ImGui::NewFrame();
}

bool UWPHostInterface::ConfirmMessage(const char* message)
{
  Log_InfoPrintf("Confirm: %s", message);

  if (!m_display)
    return true;

  const bool was_in_frame = GImGui->FrameCount != GImGui->FrameCountEnded;
  if (was_in_frame)
    ImGui::EndFrame();

  bool done = false;
  bool result = true;
  while (!done)
  {
    RunCallbacks();
    PollAndUpdate();
    if (m_fullscreen_ui_enabled)
      FullscreenUI::SetImGuiNavInputs();

    ImGui::NewFrame();
    done = FullscreenUI::DrawConfirmWindow(message, &result);
    ImGui::EndFrame();
    m_display->Render();
  }

  if (was_in_frame)
    ImGui::NewFrame();

  return result;
}

void UWPHostInterface::RunLater(std::function<void()> callback)
{
  std::unique_lock<std::mutex> lock(m_queued_callbacks_lock);
  m_queued_callbacks.push_back(std::move(callback));
}

bool UWPHostInterface::IsFullscreen() const
{
  return m_appview.IsFullScreenMode();
}

bool UWPHostInterface::SetFullscreen(bool enabled)
{
  m_dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [this, enabled]() {
    if (enabled)
      m_appview.TryEnterFullScreenMode();
    else
      m_appview.ExitFullScreenMode();
  });

  return true;
}

void UWPHostInterface::RunCallbacks()
{
  std::unique_lock<std::mutex> lock(m_queued_callbacks_lock);

  while (!m_queued_callbacks.empty())
  {
    auto callback = std::move(m_queued_callbacks.front());
    m_queued_callbacks.pop_front();
    lock.unlock();
    callback();
    lock.lock();
  }
}

void UWPHostInterface::SetWindow(const winrt::Windows::UI::Core::CoreWindow& window)
{
  m_window = window;
  m_dispatcher = m_window.Dispatcher();

  winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending({this, &UWPHostInterface::OnSuspending});

  window.Closed({this, &UWPHostInterface::OnClosed});
  window.SizeChanged({this, &UWPHostInterface::OnSizeChanged});
  window.KeyDown({this, &UWPHostInterface::OnKeyDown});
  window.KeyUp({this, &UWPHostInterface::OnKeyUp});
  window.CharacterReceived({this, &UWPHostInterface::OnCharacterReceived});
  window.PointerPressed({this, &UWPHostInterface::OnPointerPressed});
  window.PointerReleased({this, &UWPHostInterface::OnPointerPressed});
  window.PointerMoved({this, &UWPHostInterface::OnPointerMoved});
  window.PointerWheelChanged({this, &UWPHostInterface::OnPointerWheelChanged});
}

bool UWPHostInterface::SetDirectories()
{
  const auto install_location = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation();
  m_program_directory = StringUtil::WideStringToUTF8String(install_location.Path());
  if (m_program_directory.empty())
  {
    Log_ErrorPrintf("Failed to get install location");
    return false;
  }

  Log_InfoPrintf("Program directory: %s", m_program_directory.c_str());

  const auto local_location = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
  m_user_directory = StringUtil::WideStringToUTF8String(local_location.Path());
  if (m_user_directory.empty())
  {
    Log_ErrorPrintf("Failed to get user directory");
    return false;
  }

  Log_InfoPrintf("User directory: %s", m_user_directory.c_str());
  return true;
}

void UWPHostInterface::OnSuspending(const IInspectable&,
                                    const winrt::Windows::ApplicationModel::SuspendingEventArgs& args)
{
  if (IsEmulationThreadRunning())
  {
    m_shutdown_flag.store(true);
    m_emulation_thread.join();
  }
}

void UWPHostInterface::OnClosed(const IInspectable&, const winrt::Windows::UI::Core::CoreWindowEventArgs& args)
{
  if (IsEmulationThreadRunning())
  {
    m_shutdown_flag.store(true);
    m_emulation_thread.join();
  }
  args.Handled(true);
}

void UWPHostInterface::OnSizeChanged(const IInspectable&,
                                     const winrt::Windows::UI::Core::WindowSizeChangedEventArgs& args)
{
  const auto size = args.Size();
  const s32 width = static_cast<s32>(size.Width * m_display->GetWindowScale());
  const s32 height = static_cast<s32>(size.Height * m_display->GetWindowScale());
  if (IsEmulationThreadRunning())
  {
    RunLater([this, width, height]() {
      m_display->ResizeRenderWindow(width, height);
      OnHostDisplayResized();
    });
  }

  args.Handled(true);
}

void UWPHostInterface::OnKeyDown(const IInspectable&, const winrt::Windows::UI::Core::KeyEventArgs& args)
{
  const auto status = args.KeyStatus();
  if (!status.WasKeyDown && !status.IsKeyReleased && IsEmulationThreadRunning())
  {
    const HostKeyCode code = static_cast<HostKeyCode>(args.VirtualKey());
    RunLater([this, code]() {
      ImGuiIO& io = ImGui::GetIO();
      if (code < countof(io.KeysDown))
        io.KeysDown[code] = true;

      if (!io.WantCaptureKeyboard)
        HandleHostKeyEvent(code, 0, true);
    });
  }

  args.Handled(true);
}

void UWPHostInterface::OnKeyUp(const IInspectable&, const winrt::Windows::UI::Core::KeyEventArgs& args)
{
  const auto status = args.KeyStatus();
  if (status.WasKeyDown && status.IsKeyReleased && IsEmulationThreadRunning())
  {
    const HostKeyCode code = static_cast<HostKeyCode>(args.VirtualKey());
    RunLater([this, code]() {
      ImGuiIO& io = ImGui::GetIO();
      if (code < countof(io.KeysDown))
        io.KeysDown[code] = false;

      if (!io.WantCaptureKeyboard)
        HandleHostKeyEvent(code, 0, false);
    });
  }

  args.Handled(true);
}

void UWPHostInterface::OnCharacterReceived(const IInspectable&,
                                           const winrt::Windows::UI::Core::CharacterReceivedEventArgs& args)
{
  if (IsEmulationThreadRunning())
  {
    const u32 code = args.KeyCode();
    RunLater([this, code]() { ImGui::GetIO().AddInputCharacter(code); });
  }

  args.Handled(true);
}

void UWPHostInterface::OnPointerPressed(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
{
  const auto pointer = args.CurrentPoint();
  if (pointer.PointerDevice().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Mouse)
    UpdateMouseButtonState(pointer);

  args.Handled(true);
}

void UWPHostInterface::OnPointerReleased(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
{
  const auto pointer = args.CurrentPoint();
  if (pointer.PointerDevice().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Mouse)
    UpdateMouseButtonState(pointer);

  args.Handled(true);
}

void UWPHostInterface::OnPointerMoved(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
{
  const auto pointer = args.CurrentPoint();
  if (pointer.PointerDevice().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Mouse)
  {
    const auto pos = pointer.Position();
    const float x = pos.X * m_display->GetWindowScale();
    const float y = pos.Y * m_display->GetWindowScale();

    if (IsEmulationThreadRunning())
    {
      RunLater([this, x, y]() {
        m_display->SetMousePosition(static_cast<s32>(x), static_cast<s32>(y));

        if (ImGui::GetCurrentContext())
        {
          ImGuiIO& io = ImGui::GetIO();
          io.MousePos.x = x;
          io.MousePos.y = y;
        }
      });
    }

    UpdateMouseButtonState(pointer);
  }

  args.Handled(true);
}

void UWPHostInterface::OnPointerWheelChanged(const IInspectable&,
                                             const winrt::Windows::UI::Core::PointerEventArgs& args)
{
  const auto pointer = args.CurrentPoint();
  const auto properties = pointer.Properties();
  const s32 delta = properties.MouseWheelDelta();
  const bool horizontal = properties.IsHorizontalMouseWheel();

  if (IsEmulationThreadRunning())
  {
    RunLater([this, delta, horizontal]() {
      if (ImGui::GetCurrentContext())
      {
        ImGuiIO& io = ImGui::GetIO();
        const float dw = static_cast<float>(std::clamp<s32>(delta, -1, 1));
        if (horizontal)
          io.MouseWheelH = dw;
        else
          io.MouseWheel = dw;
      }
    });
  }

  args.Handled(true);
}

void UWPHostInterface::UpdateMouseButtonState(const winrt::Windows::UI::Input::PointerPoint& point)
{
  const auto properties = point.Properties();
  const bool states[3] = {properties.IsLeftButtonPressed(), properties.IsRightButtonPressed(),
                          properties.IsMiddleButtonPressed()};

  if (IsEmulationThreadRunning())
  {
    RunLater([this, states]() {
      if (!ImGui::GetCurrentContext())
        return;

      ImGuiIO& io = ImGui::GetIO();
      for (u32 i = 0; i < countof(states); i++)
      {
        if (io.MouseDown[i] == states[i])
          continue;

        io.MouseDown[i] = states[i];
        HandleHostMouseEvent(static_cast<HostMouseButton>(i), states[i]);
      }
    });
  }
}

std::optional<CommonHostInterface::HostKeyCode> UWPHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  for (const auto& it : s_key_map)
  {
    if (key_code.compare(it.second) == 0)
      return static_cast<HostKeyCode>(it.first);
  }

  return std::nullopt;
}

const char* UWPHostInterface::GetKeyCodeName(int key_code)
{
  const auto it = s_key_map.find(key_code);
  return (it != s_key_map.end()) ? it->second : nullptr;
}

void UWPHostInterface::SetImGuiKeyMap()
{
  using namespace winrt::Windows::System;

  ImGuiIO& io = ImGui::GetIO();
  io.KeyMap[ImGuiKey_Tab] = static_cast<int>(VirtualKey::Tab);
  io.KeyMap[ImGuiKey_LeftArrow] = static_cast<int>(VirtualKey::Left);
  io.KeyMap[ImGuiKey_RightArrow] = static_cast<int>(VirtualKey::Right);
  io.KeyMap[ImGuiKey_UpArrow] = static_cast<int>(VirtualKey::Up);
  io.KeyMap[ImGuiKey_DownArrow] = static_cast<int>(VirtualKey::Down);
  io.KeyMap[ImGuiKey_PageUp] = static_cast<int>(VirtualKey::PageUp);
  io.KeyMap[ImGuiKey_PageDown] = static_cast<int>(VirtualKey::PageDown);
  io.KeyMap[ImGuiKey_Home] = static_cast<int>(VirtualKey::Home);
  io.KeyMap[ImGuiKey_End] = static_cast<int>(VirtualKey::End);
  io.KeyMap[ImGuiKey_Insert] = static_cast<int>(VirtualKey::Insert);
  io.KeyMap[ImGuiKey_Delete] = static_cast<int>(VirtualKey::Delete);
  io.KeyMap[ImGuiKey_Backspace] = static_cast<int>(VirtualKey::Back);
  io.KeyMap[ImGuiKey_Space] = static_cast<int>(VirtualKey::Space);
  io.KeyMap[ImGuiKey_Enter] = static_cast<int>(VirtualKey::Enter);
  io.KeyMap[ImGuiKey_Escape] = static_cast<int>(VirtualKey::Escape);
  io.KeyMap[ImGuiKey_A] = static_cast<int>(VirtualKey::A);
  io.KeyMap[ImGuiKey_C] = static_cast<int>(VirtualKey::C);
  io.KeyMap[ImGuiKey_V] = static_cast<int>(VirtualKey::V);
  io.KeyMap[ImGuiKey_X] = static_cast<int>(VirtualKey::X);
  io.KeyMap[ImGuiKey_Y] = static_cast<int>(VirtualKey::Y);
  io.KeyMap[ImGuiKey_Z] = static_cast<int>(VirtualKey::Z);
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
  winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<UWPHostInterface>());
}
