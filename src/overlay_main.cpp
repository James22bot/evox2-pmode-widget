#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "evox2/overlay_model.hpp"
#include "evox2/tray_actions.hpp"
#include "evox2/windows_ec_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr wchar_t kWindowClass[] = L"EvoX2PModeOverlayWindow";
constexpr wchar_t kControlWindowClass[] = L"EvoX2PModeControlWindow";
constexpr wchar_t kWindowTitle[] = L"EVO-X2 P-MODE Overlay";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\EvoX2PModeOverlay.Singleton";
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kTrayId = 1;
constexpr UINT kCommandToggle = 1001;
constexpr UINT kCommandRefresh = 1002;
constexpr UINT kCommandDetails = 1003;
constexpr UINT kCommandExit = 1004;
constexpr int kAppIconResource = 101;
UINT taskbar_created_message = 0;

struct AppState {
    std::unique_ptr<evox2::windows::EcBackend> backend;
    evox2::overlay::OverlayModel model;
    evox2::tray::WriteQuarantine write_quarantine;
    evox2::tray::ModeChangeGate mode_change_gate;
    std::optional<evox2::Snapshot> startup_snapshot;
    HWND overlay_window = nullptr;
    HWND control_window = nullptr;
    HFONT font = nullptr;
    UINT font_dpi = 0;
    NOTIFYICONDATAW tray {};
    bool tray_added = false;
    bool visible = true;
    unsigned int startup_retry_countdown = 0;
    unsigned int tray_health_countdown = 24;
};

std::wstring from_utf8(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (length <= 0) {
        return L"<unreadable>";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length)
        <= 0) {
        return L"<unreadable>";
    }
    return result;
}

template <std::size_t N>
void copy_fixed(wchar_t (&destination)[N], std::wstring_view source)
{
    const std::size_t length = std::min(N - 1, source.size());
    std::wmemcpy(destination, source.data(), length);
    destination[length] = L'\0';
}

AppState* state_from(HWND window)
{
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

HICON app_icon()
{
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconResource));
    return icon != nullptr ? icon : LoadIconW(nullptr, IDI_INFORMATION);
}

void recreate_font(HWND window, AppState& state)
{
    const UINT dpi = GetDpiForWindow(window);
    if (state.font != nullptr && state.font_dpi == dpi) {
        return;
    }
    HFONT replacement = CreateFontW(
        -MulDiv(evox2::overlay::kWidgetFontPoints, static_cast<int>(dpi), 72),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    if (replacement == nullptr) {
        return;
    }
    if (state.font != nullptr) {
        DeleteObject(state.font);
    }
    state.font = replacement;
    state.font_dpi = dpi;
}

void position_overlay(HWND window)
{
    RECT work_area = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) {
        return;
    }
    const UINT dpi = GetDpiForWindow(window);
    const int width = MulDiv(evox2::overlay::kWidgetWidthLogicalPixels, static_cast<int>(dpi), 96);
    const int height = MulDiv(evox2::overlay::kWidgetHeightLogicalPixels, static_cast<int>(dpi), 96);
    const int margin = MulDiv(evox2::overlay::kOverlayMarginLogicalPixels, static_cast<int>(dpi), 96);
    const auto position = evox2::overlay::top_right_position(
        {work_area.left, work_area.top, work_area.right, work_area.bottom},
        {width, height},
        margin);
    SetWindowPos(
        window,
        HWND_BOTTOM,
        position.x,
        position.y,
        width,
        height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void update_tray(HWND window, AppState& state)
{
    const std::wstring tooltip = from_utf8(state.model.tray_tooltip());
    copy_fixed(state.tray.szTip, tooltip);
    if (state.tray_added) {
        state.tray.uFlags = NIF_TIP | NIF_SHOWTIP;
        if (!Shell_NotifyIconW(NIM_MODIFY, &state.tray)) {
            state.tray_added = false;
        }
        state.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    }
    InvalidateRect(window, nullptr, FALSE);
}

void refresh_mode(HWND window, AppState& state)
{
    if (!state.startup_snapshot.has_value() && state.startup_retry_countdown > 0) {
        --state.startup_retry_countdown;
        return;
    }
    bool changed = false;
    try {
        if (!state.startup_snapshot.has_value()) {
            state.startup_snapshot = state.backend->read_snapshot();
            state.startup_retry_countdown = 0;
            changed = state.model.set_mode(state.startup_snapshot->mode);
        } else {
            changed = state.model.set_mode(state.backend->read_mode());
        }
    } catch (const std::exception& error) {
        if (!state.startup_snapshot.has_value()) {
            state.startup_retry_countdown = 3;
        }
        changed = state.model.set_unavailable(error.what());
    }
    if (changed) {
        update_tray(window, state);
    }
}

void toggle_overlay(HWND window, AppState& state)
{
    state.visible = !state.visible;
    ShowWindow(window, state.visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (state.visible) {
        position_overlay(window);
        InvalidateRect(window, nullptr, FALSE);
    }
}

void show_details(HWND window, const AppState& state)
{
    std::wstring message;
    if (state.model.available()) {
        message = from_utf8(std::string(state.model.text()));
        if (state.startup_snapshot.has_value()) {
            message += L"\nEC firmware: ";
            message += from_utf8(state.startup_snapshot->firmware_version());
        }
    } else if (!state.write_quarantine.tripped()) {
        message = L"Status unavailable\n" + from_utf8(state.model.detail());
    } else {
        message = L"Status unavailable";
    }
    if (state.write_quarantine.tripped()) {
        message += L"\n\n";
        message += from_utf8(evox2::tray::write_quarantine_detail(state.write_quarantine));
    }
    MessageBoxW(window, message.c_str(), kWindowTitle, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

bool mode_switch_available(const AppState& state)
{
    const bool exact_firmware = state.startup_snapshot.has_value()
        && state.startup_snapshot->firmware_major == 0x01
        && state.startup_snapshot->firmware_minor == 0x08;
    return !state.mode_change_gate.in_progress() && evox2::tray::mode_switch_available(
        state.model.observed_mode(),
        exact_firmware,
        state.write_quarantine);
}

UINT mode_menu_flags(const AppState& state, evox2::PMode mode)
{
    UINT flags = MF_STRING;
    const auto current = state.model.observed_mode();
    if (!mode_switch_available(state)) {
        flags |= MF_GRAYED;
    } else if (current.has_value() && *current == mode) {
        flags |= MF_CHECKED;
    }
    return flags;
}

void request_mode_change(HWND owner, AppState& state, evox2::PMode target)
{
    HWND overlay_window = state.overlay_window;
    const auto expected = state.model.observed_mode();
    if (state.mode_change_gate.in_progress()) {
        return;
    }
    if (state.write_quarantine.tripped()) {
        const std::wstring message = from_utf8(
            evox2::tray::write_quarantine_detail(state.write_quarantine));
        MessageBoxW(owner, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
        return;
    }
    if (overlay_window == nullptr || !expected.has_value() || !mode_switch_available(state)) {
        MessageBoxW(
            owner,
            L"P-MODE-Umschalten ist nur mit verfuegbarem Status und der hardwaregeprueften EC-Firmware 1.08 moeglich.",
            kWindowTitle,
            MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return;
    }

    try {
        std::optional<evox2::ModeTransitionResult> result;
        const auto outcome = evox2::tray::run_guarded_mode_change(
            state.mode_change_gate,
            state.write_quarantine,
            [&]() {
                if (*expected == target) {
                    return true;
                }
                const std::wstring prompt = from_utf8(
                    evox2::tray::confirmation_text(*expected, target));
                return MessageBoxW(
                    owner,
                    prompt.c_str(),
                    L"P-MODE umschalten",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST) == IDYES;
            },
            [&]() {
                result = state.backend->set_mode(*expected, target);
            });
        if (outcome != evox2::tray::GuardedModeChangeOutcome::Executed || !result.has_value()) {
            return;
        }
        state.model.set_mode(result->authoritative_mode);
        update_tray(overlay_window, state);
    } catch (const evox2::EcWriteOutcomeError& error) {
        state.write_quarantine.trip(error.what());
        const std::string detail = evox2::tray::write_quarantine_detail(state.write_quarantine);
        state.model.set_unavailable(detail);
        update_tray(overlay_window, state);
        const std::wstring message = from_utf8(detail);
        MessageBoxW(owner, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
    } catch (const std::exception& error) {
        const std::string detail = "P-MODE-Umschalten fehlgeschlagen: " + std::string(error.what());
        state.model.set_unavailable(detail);
        update_tray(overlay_window, state);
        const std::wstring message = from_utf8(detail);
        MessageBoxW(owner, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
}

void show_tray_menu(HWND control_window, AppState& state)
{
    HWND overlay_window = state.overlay_window;
    if (overlay_window == nullptr) {
        return;
    }
    POINT cursor = {};
    GetCursorPos(&cursor);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    HMENU mode_menu = CreatePopupMenu();
    if (mode_menu != nullptr) {
        AppendMenuW(
            mode_menu,
            mode_menu_flags(state, evox2::PMode::Quiet),
            evox2::tray::kCommandSetQuiet,
            L"Quiet");
        AppendMenuW(
            mode_menu,
            mode_menu_flags(state, evox2::PMode::Balanced),
            evox2::tray::kCommandSetBalanced,
            L"Balanced");
        AppendMenuW(
            mode_menu,
            mode_menu_flags(state, evox2::PMode::Performance),
            evox2::tray::kCommandSetPerformance,
            L"Performance");
        const wchar_t* mode_menu_label = state.write_quarantine.tripped()
            ? L"P-MODE umschalten (gesperrt)"
            : L"P-MODE umschalten";
        if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(mode_menu), mode_menu_label)) {
            DestroyMenu(mode_menu);
            mode_menu = nullptr;
        }
    }
    AppendMenuW(menu, MF_STRING, kCommandToggle, state.visible ? L"Anzeige ausblenden" : L"Anzeige einblenden");
    AppendMenuW(menu, MF_STRING, kCommandRefresh, L"Jetzt aktualisieren");
    AppendMenuW(menu, MF_STRING, kCommandDetails, L"Details");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandExit, L"Beenden");
    SetForegroundWindow(control_window);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        cursor.x,
        cursor.y,
        0,
        control_window,
        nullptr);
    PostMessageW(control_window, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (const auto target = evox2::tray::target_mode(command); target.has_value()) {
        request_mode_change(control_window, state, *target);
        return;
    }

    switch (command) {
    case kCommandToggle:
        toggle_overlay(overlay_window, state);
        break;
    case kCommandRefresh:
        state.startup_retry_countdown = 0;
        refresh_mode(overlay_window, state);
        break;
    case kCommandDetails:
        show_details(control_window, state);
        break;
    case kCommandExit:
        DestroyWindow(overlay_window);
        break;
    default:
        break;
    }
}

bool add_tray_icon(HWND window, AppState& state)
{
    state.tray = {};
    state.tray.cbSize = sizeof(state.tray);
    state.tray.hWnd = window;
    state.tray.uID = kTrayId;
    state.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    state.tray.uCallbackMessage = kTrayCallback;
    state.tray.hIcon = app_icon();
    copy_fixed(state.tray.szTip, L"EVO-X2 P-MODE: starting");
    if (!Shell_NotifyIconW(NIM_ADD, &state.tray)) {
        return false;
    }
    state.tray_added = true;
    state.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &state.tray);
    return true;
}

bool recover_tray_icon(HWND window, AppState& state)
{
    if (state.tray.hWnd != nullptr) {
        Shell_NotifyIconW(NIM_DELETE, &state.tray);
    }
    state.tray_added = false;
    return add_tray_icon(window, state);
}

LRESULT window_procedure_impl(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    AppState* state = state_from(window);

    switch (message) {
    case WM_CREATE:
        if (state == nullptr) {
            return -1;
        }
        recreate_font(window, *state);
        if (state->font == nullptr) {
            return -1;
        }
        SetLayeredWindowAttributes(window, 0, 210, LWA_ALPHA);
        position_overlay(window);
        if (SetTimer(window, kRefreshTimer, evox2::overlay::kRefreshIntervalMilliseconds, nullptr) == 0) {
            return -1;
        }
        refresh_mode(window, *state);
        return 0;

    case WM_TIMER:
        if (state != nullptr && wparam == kRefreshTimer) {
            if (!state->tray_added) {
                if (state->control_window != nullptr && recover_tray_icon(state->control_window, *state)) {
                    update_tray(window, *state);
                }
            }
            refresh_mode(window, *state);
            if (state->visible) {
                position_overlay(window);
            }
            if (state->tray_health_countdown > 0) {
                --state->tray_health_countdown;
            }
            if (state->tray_health_countdown == 0) {
                update_tray(window, *state);
                state->tray_health_countdown = 24;
            }
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (state != nullptr) {
            recreate_font(window, *state);
            if (state->visible) {
                position_overlay(window);
            }
        }
        return 0;

    case WM_DPICHANGED:
        if (state != nullptr) {
            recreate_font(window, *state);
            if (state->visible) {
                position_overlay(window);
            }
        }
        return 0;

    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (state != nullptr) {
            PAINTSTRUCT paint = {};
            HDC context = BeginPaint(window, &paint);
            RECT client = {};
            GetClientRect(window, &client);
            HBRUSH background = CreateSolidBrush(RGB(24, 28, 34));
            FillRect(context, &client, background);
            DeleteObject(background);

            HPEN border = CreatePen(PS_SOLID, 1, RGB(55, 62, 72));
            HGDIOBJ old_pen = SelectObject(context, border);
            HGDIOBJ old_brush = SelectObject(context, GetStockObject(HOLLOW_BRUSH));
            Rectangle(context, client.left, client.top, client.right, client.bottom);
            SelectObject(context, old_brush);
            SelectObject(context, old_pen);
            DeleteObject(border);

            const auto color = state->model.color();
            SetTextColor(context, RGB(color.red, color.green, color.blue));
            SetBkMode(context, TRANSPARENT);
            HGDIOBJ old_font = SelectObject(context, state->font);
            const std::wstring text = from_utf8(state->model.text());
            DrawTextW(
                context,
                text.c_str(),
                static_cast<int>(text.size()),
                &client,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(context, old_font);
            EndPaint(window, &paint);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        if (state != nullptr) {
            if (state->tray.hWnd != nullptr) {
                Shell_NotifyIconW(NIM_DELETE, &state->tray);
            }
            state->tray_added = false;
            if (state->font != nullptr) {
                DeleteObject(state->font);
                state->font = nullptr;
            }
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    try {
        return window_procedure_impl(window, message, wparam, lparam);
    } catch (...) {
        if (message == WM_CREATE) {
            return -1;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT control_window_procedure_impl(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    AppState* state = state_from(window);

    if (message == taskbar_created_message && state != nullptr) {
        if (recover_tray_icon(window, *state) && state->overlay_window != nullptr) {
            update_tray(state->overlay_window, *state);
        }
        return 0;
    }

    switch (message) {
    case kTrayCallback:
        if (state != nullptr && state->overlay_window != nullptr) {
            const UINT event = LOWORD(lparam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                show_tray_menu(window, *state);
            } else if (event == NIN_SELECT || event == WM_LBUTTONUP) {
                toggle_overlay(state->overlay_window, *state);
            }
        }
        return 0;

    case WM_CLOSE:
        if (state != nullptr && state->overlay_window != nullptr) {
            DestroyWindow(state->overlay_window);
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK control_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    try {
        return control_window_procedure_impl(window, message, wparam, lparam);
    } catch (...) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        MessageBoxW(nullptr, L"Sichere DLL-Suche konnte nicht aktiviert werden.", kWindowTitle, MB_OK | MB_ICONERROR);
        return 5;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbar_created_message == 0) {
        MessageBoxW(nullptr, L"TaskbarCreated-Nachricht konnte nicht registriert werden.", kWindowTitle, MB_OK | MB_ICONERROR);
        return 6;
    }

    HANDLE singleton = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (singleton == nullptr) {
        MessageBoxW(nullptr, L"Single-instance mutex failed.", kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Das EVO-X2 P-MODE Overlay laeuft bereits.", kWindowTitle, MB_OK | MB_ICONINFORMATION);
        CloseHandle(singleton);
        return 0;
    }

    AppState state;
    try {
        state.backend = std::make_unique<evox2::windows::EcBackend>();
    } catch (const std::exception& error) {
        const std::wstring message = L"Overlay konnte nicht gestartet werden:\n\n" + from_utf8(error.what());
        MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return 2;
    }

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hIcon = app_icon();
    window_class.hIconSm = app_icon();
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        MessageBoxW(nullptr, L"Windows-Klasse konnte nicht registriert werden.", kWindowTitle, MB_OK | MB_ICONERROR);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return 3;
    }

    WNDCLASSEXW control_class = {};
    control_class.cbSize = sizeof(control_class);
    control_class.lpfnWndProc = control_window_procedure;
    control_class.hInstance = instance;
    control_class.lpszClassName = kControlWindowClass;
    if (RegisterClassExW(&control_class) == 0) {
        MessageBoxW(nullptr, L"Tray-Control-Klasse konnte nicht registriert werden.", kWindowTitle, MB_OK | MB_ICONERROR);
        UnregisterClassW(kWindowClass, instance);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return 3;
    }

    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kWindowClass,
        kWindowTitle,
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        evox2::overlay::kWidgetWidthLogicalPixels,
        evox2::overlay::kWidgetHeightLogicalPixels,
        nullptr,
        nullptr,
        instance,
        &state);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"Overlay-Fenster konnte nicht erstellt werden.", kWindowTitle, MB_OK | MB_ICONERROR);
        UnregisterClassW(kControlWindowClass, instance);
        UnregisterClassW(kWindowClass, instance);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return 4;
    }

    state.overlay_window = window;
    HWND control_window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kControlWindowClass,
        L"",
        WS_POPUP,
        -32000,
        -32000,
        1,
        1,
        nullptr,
        nullptr,
        instance,
        &state);
    if (control_window == nullptr) {
        MessageBoxW(nullptr, L"Tray-Control-Fenster konnte nicht erstellt werden.", kWindowTitle, MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        UnregisterClassW(kControlWindowClass, instance);
        UnregisterClassW(kWindowClass, instance);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return 4;
    }
    state.control_window = control_window;

    bool message_filters_ready = true;
    if (!ChangeWindowMessageFilterEx(control_window, taskbar_created_message, MSGFLT_ALLOW, nullptr)) {
        message_filters_ready = false;
    }
    if (!ChangeWindowMessageFilterEx(control_window, kTrayCallback, MSGFLT_ALLOW, nullptr)) {
        message_filters_ready = false;
    }
    if (!message_filters_ready) {
        MessageBoxW(nullptr, L"Tray-Nachrichtenfilter konnte nicht sicher eingerichtet werden.", kWindowTitle, MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        DestroyWindow(control_window);
        UnregisterClassW(kControlWindowClass, instance);
        UnregisterClassW(kWindowClass, instance);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return 5;
    }

    if (!add_tray_icon(control_window, state)) {
        MessageBoxW(nullptr, L"Tray-Symbol konnte nicht erstellt werden.", kWindowTitle, MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        DestroyWindow(control_window);
        UnregisterClassW(kControlWindowClass, instance);
        UnregisterClassW(kWindowClass, instance);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return 5;
    }
    update_tray(window, state);

    ShowWindow(control_window, SW_SHOWNOACTIVATE);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    position_overlay(window);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (IsWindow(control_window)) {
        DestroyWindow(control_window);
    }
    UnregisterClassW(kControlWindowClass, instance);
    UnregisterClassW(kWindowClass, instance);
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return static_cast<int>(message.wParam);
}
