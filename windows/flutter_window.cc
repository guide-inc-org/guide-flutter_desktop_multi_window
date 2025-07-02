//
// Created by yangbin on 2022/1/11.
//
#include <windows.h>
#include <cmath> // For sqrt
#include "flutter_window.h"
#include "multi_window_manager.h"
#include "flutter_windows.h"

#include "tchar.h"

#include "resource.h"

#include <iostream>
#include <utility>

#include "include/desktop_multi_window/desktop_multi_window_plugin.h"
#include "multi_window_plugin_internal.h"

#include <CommCtrl.h>
#pragma comment(lib, "Comctl32.lib")

/// RustDesk deps using method channel
// #include <bitsdojo_window_windows/bitsdojo_window_plugin.h>
#include <url_launcher_windows/url_launcher_windows.h>
// #include <window_size/window_size_plugin.h>
#include <texture_rgba_renderer/texture_rgba_renderer_plugin_c_api.h>
// #include <window_manager/window_manager_plugin.h>
// #include <screen_retriever/screen_retriever_plugin.h>
// #include <tray_manager/tray_manager_plugin.h>

void RustDeskRegisterPlugins(flutter::PluginRegistry* registry) {
    // BitsdojoWindowPluginRegisterWithRegistrar(
    //    registry->GetRegistrarForPlugin("BitsdojoWindowPlugin"));
    UrlLauncherWindowsRegisterWithRegistrar(
        registry->GetRegistrarForPlugin("UrlLauncherWindows"));
    // WindowSizePluginRegisterWithRegistrar(registry->GetRegistrarForPlugin("WindowSizePlugin"));
    TextureRgbaRendererPluginCApiRegisterWithRegistrar(registry->GetRegistrarForPlugin("TextureRgbaRendererPlugin"));
    // WindowManagerPluginRegisterWithRegistrar(
    //     registry->GetRegistrarForPlugin("WindowManagerPlugin"));
    // ScreenRetrieverPluginRegisterWithRegistrar(
    //   registry->GetRegistrarForPlugin("ScreenRetrieverPlugin"));
    // TrayManagerPluginRegisterWithRegistrar(
    //  registry->GetRegistrarForPlugin("TrayManagerPlugin"));
}

bool IsWindows11OrGreater() {
  DWORD dwVersion = 0;
  DWORD dwBuild = 0;

#pragma warning(push)
#pragma warning(disable : 4996)
  dwVersion = GetVersion();
  // Get the build number.
  if (dwVersion < 0x80000000)
    dwBuild = (DWORD)(HIWORD(dwVersion));
#pragma warning(pop)

  return dwBuild >= 22000;
}

namespace {

WindowCreatedCallback _g_window_created_callback = nullptr;

TCHAR kFlutterWindowClassName[] = _T("RustdeskMultiWindow");

int32_t class_registered_ = 0;

void RegisterWindowClass(WNDPROC wnd_proc) {
  if (class_registered_ == 0) {
    WNDCLASS window_class{};
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = kFlutterWindowClassName;
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.cbClsExtra = 0;
    window_class.cbWndExtra = 0;
    window_class.hInstance = GetModuleHandle(nullptr);
    window_class.hIcon =
        LoadIcon(window_class.hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    window_class.lpszMenuName = nullptr;
    window_class.lpfnWndProc = wnd_proc;
    window_class.hbrBackground = NULL;
    RegisterClass(&window_class);
  }
  class_registered_++;
}

void UnregisterWindowClass() {
  class_registered_--;
  if (class_registered_ != 0) {
    return;
  }
  UnregisterClass(kFlutterWindowClassName, nullptr);
}

// Scale helper to convert logical scaler values to physical using passed in
// scale factor
inline int Scale(int source, double scale_factor) {
  return static_cast<int>(source * scale_factor);
}

using EnableNonClientDpiScaling = BOOL __stdcall(HWND hwnd);

// Dynamically loads the |EnableNonClientDpiScaling| from the User32 module.
// This API is only needed for PerMonitor V1 awareness mode.
void EnableFullDpiSupportIfAvailable(HWND hwnd) {
  HMODULE user32_module = LoadLibraryA("User32.dll");
  if (!user32_module) {
    return;
  }
  auto enable_non_client_dpi_scaling =
      reinterpret_cast<EnableNonClientDpiScaling *>(
          GetProcAddress(user32_module, "EnableNonClientDpiScaling"));
  if (enable_non_client_dpi_scaling != nullptr) {
    enable_non_client_dpi_scaling(hwnd);
    FreeLibrary(user32_module);
  }
}

}

FlutterWindow::FlutterWindow(
    HWND parent,
    int64_t id,
    std::string args,
    const std::shared_ptr<FlutterWindowCallback> &callback
) : callback_(callback), id_(id), window_handle_(nullptr), scale_factor_(1) {
  RegisterWindowClass(FlutterWindow::WndProc);

  const POINT target_point = {static_cast<LONG>(10),
                              static_cast<LONG>(10)};
  HMONITOR monitor = MonitorFromPoint(target_point, MONITOR_DEFAULTTONEAREST);
  UINT dpi = FlutterDesktopGetDpiForMonitor(monitor);
  scale_factor_ = dpi / 96.0;
  this->pixel_ratio_ = scale_factor_;

  HWND window_handle = CreateWindow(
      kFlutterWindowClassName, L"", WS_OVERLAPPEDWINDOW,
      Scale(target_point.x, scale_factor_), Scale(target_point.y, scale_factor_),
      Scale(1280, scale_factor_), Scale(720, scale_factor_),
      nullptr, nullptr, GetModuleHandle(nullptr), this);

  RECT frame;
  GetClientRect(window_handle, &frame);
  flutter::DartProject project(L"data");
  project.set_dart_entrypoint_arguments({"multi_window", std::to_string(id), args});
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    std::cerr << "Failed to setup FlutterViewController." << std::endl;
  }
  auto view_handle = flutter_controller_->view()->GetNativeWindow();
  SetParent(view_handle, window_handle);
  if (view_handle)
    SetWindowSubclass(view_handle, SubclassProc, 1, 0);
  MoveWindow(view_handle, 0, 0, frame.right - frame.left, frame.bottom - frame.top, true);

  RustDeskRegisterPlugins(flutter_controller_->engine());
  InternalMultiWindowPluginRegisterWithRegistrar(
      flutter_controller_->engine()->GetRegistrarForPlugin("DesktopMultiWindowPlugin"));
  window_channel_ = WindowChannel::RegisterWithRegistrar(
      flutter_controller_->engine()->GetRegistrarForPlugin("DesktopMultiWindowPlugin"), id_);

  if (_g_window_created_callback) {
    _g_window_created_callback(flutter_controller_.get(), std::move(args));
  }

  // hide the window when created.
  ShowWindow(window_handle, SW_HIDE);
}

// static
FlutterWindow *FlutterWindow::GetThisFromHandle(HWND window) noexcept {
  return reinterpret_cast<FlutterWindow *>(
      GetWindowLongPtr(window, GWLP_USERDATA));
}

LRESULT CALLBACK FlutterWindow::SubclassProc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
  HWND parentHwnd = GetParent(hwnd);
  auto window = reinterpret_cast<FlutterWindow *>(GetWindowLongPtr(parentHwnd, GWLP_USERDATA));
  if (!window) {
    // If somehow there's no valid pointer, call DefSubclassProc
    return DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_INPUTLANGCHANGE:
      window->EmitEvent("onchangekeyboard");
      break;
    case WM_IME_NOTIFY:
      if(wparam == IMN_SETCONVERSIONMODE)
        window->EmitEvent("onchangekeyboard");
      break;

    case WM_DESTROY:
      // You can optionally remove the subclass here if needed
      RemoveWindowSubclass(hwnd, SubclassProc, uIdSubclass);
      break;
  }

  return DefSubclassProc(hwnd, msg, wparam, lparam);
}

// static
LRESULT CALLBACK FlutterWindow::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto window_struct = reinterpret_cast<CREATESTRUCT *>(lparam);
    SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window_struct->lpCreateParams));

    auto that = static_cast<FlutterWindow *>(window_struct->lpCreateParams);
    EnableFullDpiSupportIfAvailable(window);
    that->window_handle_ = window;
  } else if (FlutterWindow *that = GetThisFromHandle(window)) {
    return that->MessageHandler(window, message, wparam, lparam);
  }

  return DefWindowProc(window, message, wparam, lparam);
}

RECT lastRect = {0, 0, 0, 0};
POINT lastPoint = {0, 0};
int deltaX = 0;
int deltaY = 0;


bool IsWindowCovered(HWND hwnd)
{
  RECT rect;
  if (!GetWindowRect(hwnd, &rect) || (!IsWindow(hwnd)))
  {
    return true; // Assume covered if we can't get the rect
  }

  // Check key points: Top-left and center
  POINT points[] = {
      {rect.left + 1, rect.top + 1},                                // top-left
      {rect.right - 1, rect.top + 1},                               // top -right
      {rect.left + 1, rect.bottom - 1},                             // bottom-left
      {rect.right - 1, rect.bottom - 1}                             // bottom-right
  };

  
  for (const auto& pt : points) {
        HWND topHwnd = WindowFromPoint(pt);
        if (topHwnd != hwnd && !IsChild(hwnd, topHwnd)) {
            return true; // Some other window is above at this point
        }
  }


  return false;
}

bool IsWindowEdgeCovered(HWND hwnd, int edge)
{
    RECT rect;
    if (!GetWindowRect(hwnd, &rect) || !IsWindow(hwnd)) {
        return true; // Assume covered if we can't get the rect
    }

    const int NUM_POINTS = 5; // Check 5 points along the edge
    POINT points[NUM_POINTS];
    
    // Calculate points along the specified edge
    switch (edge) {
        case 0: // Left edge
            for (int i = 0; i < NUM_POINTS; i++) {
                points[i].x = rect.left + 1;
                points[i].y = rect.top + (rect.bottom - rect.top) * i / (NUM_POINTS - 1);
            }
            break;
        case 1: // Top edge
            for (int i = 0; i < NUM_POINTS; i++) {
                points[i].x = rect.left + (rect.right - rect.left) * i / (NUM_POINTS - 1);
                points[i].y = rect.top + 1;
            }
            break;
        case 2: // Right edge
            for (int i = 0; i < NUM_POINTS; i++) {
                points[i].x = rect.right - 1;
                points[i].y = rect.top + (rect.bottom - rect.top) * i / (NUM_POINTS - 1);
            }
            break;
        case 3: // Bottom edge
            for (int i = 0; i < NUM_POINTS; i++) {
                points[i].x = rect.left + (rect.right - rect.left) * i / (NUM_POINTS - 1);
                points[i].y = rect.bottom - 1;
            }
            break;
        default:
            return false;
    }
    
    // Check if all points on the edge are covered
    for (int i = 0; i < NUM_POINTS; i++) {
        HWND topHwnd = WindowFromPoint(points[i]);
        if (topHwnd == hwnd || IsChild(hwnd, topHwnd)) {
            return false; // At least one point on the edge is not covered
        }
    }
    
    return true; // All points on the edge are covered
}

bool IsMaximizedCheck(HWND hwnd)
{
  RECT windowRect;
  if (GetWindowRect(hwnd, &windowRect))
  {
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {0};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hMonitor, &mi))
    {
      RECT workRect = mi.rcWork;
      // Optionally allow for a tolerance if needed:
      const int TOLERANCE = 2; // pixels
      if (abs(windowRect.left - workRect.left) <= TOLERANCE &&
          abs(windowRect.top - workRect.top) <= TOLERANCE)
      {
        return true;
      }
    }
  }
  return false;
}

LRESULT FlutterWindow::MessageHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result = flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam, lparam);
    if (result) {
      return *result;
    }
  }

  auto child_content_ = flutter_controller_ ? flutter_controller_->view()->GetNativeWindow() : nullptr;

  switch (message) {
    case WM_NCCALCSIZE: {
        // This must always be first or else the one of other two ifs will execute
        //  when window is in full screen and we don't want that
        if (wparam && IsFullscreen()) {
            // Note:
            // I dont know why we should -3 on the bottom. 
            //
            // NCCALCSIZE_PARAMS* sz = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
            // sz->rgrc[0].bottom -= 3;
            return 0;
        }
        // This must always be before handling title_bar_style_ == "hidden" so
        //  the if TitleBarStyle.hidden doesn't get executed.
        if (wparam && IsFrameless()) {
            // Add borders when maximized so app doesn't get cut off.
            if (IsMaximized()) {
                adjustNCCALCSIZE(hwnd, reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam));
            }
            // This cuts the app at the bottom by one pixel but that's necessary to
            // prevent jitter when resizing the app
            NCCALCSIZE_PARAMS* sz = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
            sz->rgrc[0].bottom += 1;
            return 0;
        }
        if (wparam && this->title_bar_style_ == "hidden") {
            // Add pixel to the top border when maximized so the app isn't cut off
            if (this->IsMaximized()) {
                adjustNCCALCSIZE(hwnd, reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam));
            }
            else {
                NCCALCSIZE_PARAMS* sz = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                // on windows 11, if set to 0, there's a white line at the top
                // of the app and I've yet to find a way to remove that.
                sz->rgrc[0].top += IsWindows11OrGreater() ? 1: 0;
            }

            // Previously (WVR_HREDRAW | WVR_VREDRAW), but returning 0 or 1 doesn't
            // actually break anything so I've set it to 0. Unless someone pointed a
            // problem in the future.
            return 0;
        }
        break;
    }
    case WM_SHOWWINDOW: {
      if (wparam == TRUE) {
        EmitEvent("show");
      } else {
        EmitEvent("hide");
      }
      break;
    }
    case WM_FONTCHANGE: {
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
    }
    case WM_DESTROY:
      // prevent crash
      if (!destroyed_) {
        destroyed_ = true;
        // Give onDestroy callback to Flutter to close window gracefully
        tryInvokeChannelOnDestroy();
        if (auto callback = callback_.lock()) {
          callback->OnWindowDestroy(id_);
        }
      }
      return 0;
    case WM_CLOSE: {
      EmitEvent("close");
      if (this->IsPreventClose()) {
        return -1;
      }
      if (auto callback = callback_.lock()) {
        callback->OnWindowClose(id_);
      }
      break;
    }
    case WM_GETMINMAXINFO: {
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
      // For the special "unconstrained" values, leave the defaults.
      if (this->minimum_size_.x != 0)
        info->ptMinTrackSize.x = static_cast<LONG> (this->minimum_size_.x * this->pixel_ratio_);
      if (this->minimum_size_.y != 0)
        info->ptMinTrackSize.y = static_cast<LONG> (this->minimum_size_.y * this->pixel_ratio_);
      
      if (this->maximum_size_.x != -1) {
        info->ptMaxTrackSize.x = static_cast<LONG>(this->maximum_size_.x * this->pixel_ratio_);
        if (this->maximum_size_.y == -1) {
          // fix: https://guide.backlog.com/view/SBIFX-7951
          NCCALCSIZE_PARAMS* sz = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
          LONG t = 0;
          HMONITOR monitor = MonitorFromRect(&sz->rgrc[0], MONITOR_DEFAULTTONEAREST);
          HMONITOR mo = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
          if (monitor != NULL) {
            MONITORINFO monitorInfo;
            MONITORINFO mi;
            monitorInfo.cbSize = sizeof(MONITORINFO);
            mi.cbSize = sizeof(MONITORINFO);
            if (GetMonitorInfo(monitor, &monitorInfo)) {
              info->ptMaxPosition.x = monitorInfo.rcWork.left;
            }
            if (GetMonitorInfo(mo, &mi)) {
              t = sz->rgrc[0].top / 2 - mi.rcWork.top;
            }
            info->ptMaxTrackSize.y = mi.rcWork.bottom + t; // Full height excluding taskbar
          }
        }
      }
      if (this->maximum_size_.y != -1)
        info->ptMaxTrackSize.y = static_cast<LONG>(this->maximum_size_.y * this->pixel_ratio_);
      break;
    }
    case WM_DPICHANGED: {
      this->pixel_ratio_ = (float) LOWORD(wparam) / USER_DEFAULT_SCREEN_DPI;

      auto newRectSize = reinterpret_cast<RECT *>(lparam);
      LONG newWidth = newRectSize->right - newRectSize->left;
      LONG newHeight = newRectSize->bottom - newRectSize->top;

      SetWindowPos(hwnd, nullptr, newRectSize->left, newRectSize->top, newWidth,
                   newHeight, SWP_NOZORDER | SWP_NOACTIVATE);

      ForceChildRefresh();

      return 0;
    }
    case WM_SIZE: {
      RECT rect;
      GetClientRect(window_handle_, &rect);
      if (child_content_ != nullptr) {
        // Size and position the child window.
        MoveWindow(child_content_, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top, TRUE);
      }
      LONG_PTR gwlStyle =
          GetWindowLongPtr(window_handle_, GWL_STYLE);
      if ((gwlStyle & (WS_CAPTION | WS_THICKFRAME)) == 0 &&
          wparam == SIZE_MAXIMIZED) {
          EmitEvent("enter-full-screen");
          this->last_state = STATE_FULLSCREEN_ENTERED;
      }
      else if (this->last_state == STATE_FULLSCREEN_ENTERED &&
          wparam == SIZE_RESTORED) {
          ForceChildRefresh();
          EmitEvent("leave-full-screen");
          last_state = STATE_NORMAL;
      }
      else if (wparam == SIZE_MAXIMIZED) {
          EmitEvent("maximize");
          last_state = STATE_MAXIMIZED;
      }
      else if (wparam == SIZE_MINIMIZED) {
          EmitEvent("minimize");
          last_state = STATE_MINIMIZED;
      }
      else if (wparam == SIZE_RESTORED) {
          if (last_state == STATE_MAXIMIZED) {
              EmitEvent("unmaximize");
              last_state = STATE_NORMAL;
          }
          else if (last_state == STATE_MINIMIZED) {
              EmitEvent("restore");
              last_state = STATE_NORMAL;
          }
      }
      EmitEvent("resized");
      break;
    }

    case WM_MOVE:
      EmitEvent("moved");
      lastRect = {0, 0, 0, 0};
      lastPoint = {0, 0};
      deltaX = 0;
      deltaY = 0;
      break;

    case WM_ACTIVATE: {
      if (child_content_ != nullptr) {
        SetFocus(child_content_);
      }
      return 0;
    }
    case WM_WINDOWPOSCHANGING: {
      WINDOWPOS *pPos = (WINDOWPOS *)lparam;

      // Check if velocity is slow enough for snapping

      if (is_prevent_focus_) {
        // Check if the window is being brought to the top (SWP_NOZORDER is NOT
        // set)
        if (!(pPos->flags & SWP_NOZORDER)) {
          // Prevent the window from being brought to the top
          pPos->flags |= SWP_NOZORDER;
        }
      }
      break;
    }
    case WM_SYSCOMMAND: {
      // Check if the command is for minimizing the window
      if (is_prevent_focus_ && (wparam & 0xFFF0) == SC_MINIMIZE) {
        // Prevent the window from minimizing
        return 0;
      }
    }
    case WM_SIZING: {
      EmitEvent("resize");
      break;
    }
    case WM_WINDOWPOSCHANGED: {
      lastRect = {0, 0, 0, 0};
      lastPoint = {0, 0};
      deltaX = 0;
      deltaY = 0;
      break;
    }
    case WM_MOVING: {
      // Get pointer to the window's RECT
      RECT* rect = reinterpret_cast<RECT*>(lparam);
  
      // Update delta based on mouse position
      if (lastPoint.x == 0 && lastPoint.y == 0) {
          GetCursorPos(&lastPoint);
      }
      POINT currentPoint;
      GetCursorPos(&currentPoint);
      deltaX += currentPoint.x - lastPoint.x;
      deltaY += currentPoint.y - lastPoint.y;
      lastPoint = currentPoint;
  
      // Clone rect for calculation
      RECT cloneRect = *rect;
  
      // Set snap threshold and margin (in pixel_ratio_)
      int snapThreshold = int(round(10 * pixel_ratio_));
      int baseMarginHorizontal = -int(round(4 / pixel_ratio_));
      int baseMarginVertical = -int(round(4 / pixel_ratio_));
  
      //Size of the window
      int width = cloneRect.right - cloneRect.left;
      int height = cloneRect.bottom - cloneRect.top;
  
      // Tracking snap candidates
      bool snapCandidateX = false;
      bool snapCandidateY = false;
      int candidateSnapX = cloneRect.left;
      int candidateSnapY = cloneRect.top;
      int minDx = snapThreshold + 1; // Minimum difference for snap horizontally
      int minDy = snapThreshold + 1; // Minimum difference for snap vertically
  
      //If the delta is too large, break the loop
      if (abs(deltaX) > snapThreshold * 2 || abs(deltaY) > snapThreshold * 2) {
          break;
      }
  
      // If the movement is small (less than 5 pixels), update cloneRect based on delta
      if (abs(lastRect.left - cloneRect.left) < 5 && abs(lastRect.top - cloneRect.top) < 5) {
          cloneRect.left = lastRect.left + deltaX;
          cloneRect.top = lastRect.top + deltaY;
          cloneRect.right = cloneRect.left + width;
          cloneRect.bottom = cloneRect.top + height;
      }
  
      MultiWindowManager *manager = MultiWindowManager::Instance();
      if (manager) {
          for (auto &w : manager->windows_) {
              HWND otherHwnd = w.second->GetWindowHandle();
              // Skip its own window, "Toast", "Dialog" or windows that are not visible, covered
              wchar_t title[256];
              GetWindowText(otherHwnd, title, 256);
              std::wstring titleW(title);
              if (otherHwnd == hwnd || titleW == L"Toast" || titleW == L"Dialog" ||
                  !IsWindowVisible(otherHwnd)) {
                  continue;
              }
              RECT otherRect;
              GetWindowRect(otherHwnd, &otherRect);
              bool isMenu = (titleW == L"FLUTTERVIEW");
  
              // Set margin for each window
              int marginHorizontal = baseMarginHorizontal;
              int marginVertical = baseMarginVertical;
              if (isMenu) {
                  marginHorizontal -= int(round(5 / pixel_ratio_));
              }
  
              // Check snapping horizontally:
              // 1. If the left edge of cloneRect is close to the right edge of another window.
              int diff = abs(cloneRect.left - otherRect.right);
              if (diff < snapThreshold &&
                  (cloneRect.top <= otherRect.bottom) &&
                  (cloneRect.bottom >= otherRect.top) &&
                  !IsWindowEdgeCovered(otherHwnd, 2)) { // Check if right edge is covered
                  int candidate = otherRect.right - min(marginHorizontal, snapThreshold);
                  if (diff < minDx) {
                      minDx = diff;
                      candidateSnapX = candidate;
                      snapCandidateX = true;
                  }
              }
              // 2. If the right edge of cloneRect is close to the left edge of another window.
              diff = abs(cloneRect.right - otherRect.left);
              if (diff < snapThreshold &&
                  (cloneRect.top <= otherRect.bottom) &&
                  (cloneRect.bottom >= otherRect.top) &&
                  !IsWindowEdgeCovered(otherHwnd, 0)) { // Check if left edge is covered
                  int candidate = otherRect.left - width + min(marginHorizontal, snapThreshold);
                  if (diff < minDx) {
                      minDx = diff;
                      candidateSnapX = candidate;
                      snapCandidateX = true;
                  }
              }
  
              // Check snapping vertically:
              // 1. If the top edge of cloneRect is close to the bottom edge of another window.
              diff = abs(cloneRect.top - otherRect.bottom);
              if (diff < snapThreshold &&
                  (cloneRect.left <= otherRect.right) &&
                  (cloneRect.right >= otherRect.left) &&
                  !IsWindowEdgeCovered(otherHwnd, 3)) { // Check if bottom edge is covered
                  int candidate = otherRect.bottom - min(marginVertical, snapThreshold);
                  if (diff < minDy) {
                      minDy = diff;
                      candidateSnapY = candidate;
                      snapCandidateY = true;
                  }
              }
              // 2. If the bottom edge of cloneRect is close to the top edge of another window.
              diff = abs(cloneRect.bottom - otherRect.top);
              if (diff < snapThreshold &&
                  (cloneRect.left <= otherRect.right) &&
                  (cloneRect.right >= otherRect.left) &&
                  !IsWindowEdgeCovered(otherHwnd, 1)) { // Check if top edge is covered
                  int candidate = otherRect.top - height + min(marginVertical, snapThreshold);
                  if (diff < minDy) {
                      minDy = diff;
                      candidateSnapY = candidate;
                      snapCandidateY = true;
                  }
              }
          } // end for each window
      }
  
      // Set unsnap threshold: if the position of cloneRect is too far from the candidate, then unsnap
      const int unsnapThreshold = snapThreshold + 5;
      bool finalSnapX = snapCandidateX;
      bool finalSnapY = snapCandidateY;
      if (snapCandidateX) {
          if (abs(cloneRect.left - candidateSnapX) > unsnapThreshold) {
              finalSnapX = false;
          }
      }
      if (snapCandidateY) {
          if (abs(cloneRect.top - candidateSnapY) > unsnapThreshold) {
              finalSnapY = false;
          }
      }
  
      // Update RECT based on final snap result
      if (finalSnapX || finalSnapY) {
          rect->left = finalSnapX ? candidateSnapX : cloneRect.left;
          rect->top  = finalSnapY ? candidateSnapY : cloneRect.top;
          rect->right = rect->left + width;
          rect->bottom = rect->top + height;
          if (lastRect.left == 0 && lastRect.top == 0) {
              lastRect = *rect;
          }
      } else {
          // If not snap, keep cloneRect and reset tracking variables.
          *rect = cloneRect;
          lastRect = {0, 0, 0, 0};
          lastPoint = {0, 0};
          deltaX = 0;
          deltaY = 0;
      }
      break;
  }
  


    case WM_NCACTIVATE: {
        char* eventName;
        if (wparam == TRUE) {
            eventName = "focus";
        }
        else {
            eventName = "blur";
        }
        EmitEvent(eventName);
        break;
    }
    case WM_ERASEBKGND: {
        if(is_reset_bg_) break;
        HDC hdc = (HDC) wparam;
        HBRUSH brush = CreateSolidBrush(this->window_background_color_);
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(hdc, &rect, brush);
        DeleteObject(brush);
        return 1; // Background has been erased
    }

    default: break;
  }

  return DefWindowProc(window_handle_, message, wparam, lparam);
}

void FlutterWindow::tryInvokeChannelOnDestroy()
{
  if (window_channel_) {
      auto args = flutter::EncodableValue(flutter::EncodableMap());
      window_channel_->InvokeMethod(0, "onDestroy", &args);
      window_channel_->SetMethodCallHandler(nullptr);
      window_channel_.reset();
  }
}

void FlutterWindow::EmitEvent(const char* eventName)
{
    auto params = flutter::EncodableMap();
    params.emplace(flutter::EncodableValue("eventName"), flutter::EncodableValue(eventName));
    auto args = flutter::EncodableValue(std::move(params));
    window_channel_->InvokeMethod(0, "onEvent", &args);
}

void FlutterWindow::Destroy() {
  tryInvokeChannelOnDestroy();
  if (window_channel_) {
    window_channel_ = nullptr;
  }
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }
  if (window_handle_) {
    DestroyWindow(window_handle_);
    window_handle_ = nullptr;
  }
}

FlutterWindow::~FlutterWindow() {
  this->Destroy();
  if (window_handle_) {
    std::cout << "window_handle leak." << std::endl;
  }
  UnregisterWindowClass();
}

void DesktopMultiWindowSetWindowCreatedCallback(WindowCreatedCallback callback) {
  _g_window_created_callback = callback;
}