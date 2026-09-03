#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <psapi.h>


struct ClickerConfig {
    bool enabled = false;
    double cps_min = 21.0;
    double cps_max = 21.0;
    bool only_ingame = true;
    int trigger_button = VK_LBUTTON;
};

ClickerConfig g_cfg;
std::mutex g_cfg_mutex;
HHOOK g_hook = nullptr;
DWORD g_hook_thread_id = 0;
std::atomic<bool> g_phys_trigger{false};
std::atomic<bool> g_running{true};
std::atomic<bool> g_mc_focused{false};
std::atomic<int> g_trigger_vk{VK_LBUTTON};
std::atomic<bool> g_needs_redraw{true};


double uniform_rand(double lo, double hi) {
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(rng);
}

bool key_held(int vk) {
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool physical_button_held() {
    return g_hook ? g_phys_trigger.load(std::memory_order_relaxed) : key_held(g_trigger_vk.load(std::memory_order_relaxed));
}

void send_click_down() {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input.mi.dx = input.mi.dy = 0;
    input.mi.mouseData = 0;
    input.mi.time = 0;
    input.mi.dwExtraInfo = 0;
    SendInput(1, &input, sizeof(INPUT));
}

void send_click_up() {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    input.mi.dx = input.mi.dy = 0;
    input.mi.mouseData = 0;
    input.mi.time = 0;
    input.mi.dwExtraInfo = 0;
    SendInput(1, &input, sizeof(INPUT));
}

void precise_delay_ms(double ms) {
    if (ms <= 0) return;
    auto start = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
        if (elapsed >= ms) break;
        double remain = ms - elapsed;
        if (remain > 2.0) Sleep(1);
        else std::this_thread::yield();
    }
}


bool is_minecraft_active() {
    static DWORD s_last_pid = 0;
    static bool s_last_is_mc = false;

    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0) return false;

    if (pid == s_last_pid) {
        return s_last_is_mc;
    }

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        s_last_pid = pid;
        s_last_is_mc = false;
        return false;
    }

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool isMinecraft = false;
    if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
        std::wstring fullPath(path);
        size_t pos = fullPath.find_last_of(L"\\/");
        std::wstring exeName = (pos == std::wstring::npos) ? fullPath : fullPath.substr(pos + 1);
        std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::towlower);
        isMinecraft = (exeName == L"minecraft.windows.exe" ||
                       exeName == L"minecraft.exe" ||
                       exeName == L"javaw.exe" ||
                       exeName == L"java.exe");
    }
    CloseHandle(hProcess);

    s_last_pid = pid;
    s_last_is_mc = isMinecraft;
    return isMinecraft;
}


LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && lParam) {
        if (wParam == WM_MOUSEMOVE) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (!(info->flags & LLMHF_INJECTED)) {
            int trigger = g_trigger_vk.load(std::memory_order_relaxed);
            bool set_state = false;
            bool new_state = false;

            if (trigger == VK_LBUTTON && wParam == WM_LBUTTONDOWN) { set_state = true; new_state = true; }
            else if (trigger == VK_LBUTTON && wParam == WM_LBUTTONUP) { set_state = true; new_state = false; }
            else if (trigger == VK_RBUTTON && wParam == WM_RBUTTONDOWN) { set_state = true; new_state = true; }
            else if (trigger == VK_RBUTTON && wParam == WM_RBUTTONUP) { set_state = true; new_state = false; }
            else if (trigger == VK_XBUTTON1 && wParam == WM_XBUTTONDOWN) {
                if (HIWORD(info->mouseData) == 1) { set_state = true; new_state = true; }
            }
            else if (trigger == VK_XBUTTON1 && wParam == WM_XBUTTONUP) {
                if (HIWORD(info->mouseData) == 1) { set_state = true; new_state = false; }
            }
            else if (trigger == VK_XBUTTON2 && wParam == WM_XBUTTONDOWN) {
                if (HIWORD(info->mouseData) == 2) { set_state = true; new_state = true; }
            }
            else if (trigger == VK_XBUTTON2 && wParam == WM_XBUTTONUP) {
                if (HIWORD(info->mouseData) == 2) { set_state = true; new_state = false; }
            }

            if (set_state) {
                bool prev = g_phys_trigger.exchange(new_state, std::memory_order_relaxed);
                if (prev != new_state) {
                    g_needs_redraw.store(true, std::memory_order_relaxed);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}


void hook_thread_func() {
    g_hook_thread_id = GetCurrentThreadId();
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);

    MSG msg;
    while (g_running.load(std::memory_order_relaxed)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    }

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
}


struct Scheduler {
    std::chrono::steady_clock::time_point start;
    double next_expected = 0.0;

    void reset() { start = std::chrono::steady_clock::now(); next_expected = 0.0; }

    std::pair<double,double> next(double up, double down) {
        double total = up + down;
        next_expected += total;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
        double needed = next_expected - elapsed;
        if (needed < 10.0) { needed = 10.0; next_expected = elapsed + 10.0; }
        double ratio = total > 0 ? up / total : 0.9;
        double comp_up = std::round(needed * ratio);
        double comp_down = std::round(needed - comp_up);
        if (comp_up < 0) comp_up = 0;
        if (comp_down < 0) comp_down = 0;
        return { comp_up, comp_down };
    }
};


void clicker_loop() {
    Scheduler sched;
    bool was_clicking = false;
    while (g_running.load(std::memory_order_relaxed)) {
        ClickerConfig cfg;
        {
            std::lock_guard<std::mutex> lock(g_cfg_mutex);
            cfg = g_cfg;
            g_trigger_vk.store(cfg.trigger_button, std::memory_order_relaxed);
        }
        bool focus_ok = cfg.only_ingame ? g_mc_focused.load(std::memory_order_relaxed) : true;
        bool hold = physical_button_held();
        bool should = cfg.enabled && focus_ok && hold;

        if (should) {
            if (!was_clicking) { sched.reset(); was_clicking = true; }
            double lo = std::min(cfg.cps_min, cfg.cps_max);
            double hi = std::max(cfg.cps_min, cfg.cps_max);
            double cps = (lo == hi) ? lo : (lo + uniform_rand(0.0, 1.0) * (hi - lo));
            cps = std::clamp(cps, 1.0, 50.0);

            double total = 1000.0 / cps;
            double down = std::clamp(total * 0.25, 3.0, 25.0);
            double up = std::max(1.0, total - down);
            auto [comp_up, comp_down] = sched.next(up, down);

            send_click_up();
            precise_delay_ms(comp_up);
            if (!physical_button_held()) continue;
            send_click_down();
            precise_delay_ms(comp_down);
        } else {
            if (was_clicking) { send_click_up(); was_clicking = false; }
            Sleep(1);
        }
    }
    if (was_clicking) send_click_up();
}


void focus_poll_loop() {
    while (g_running.load(std::memory_order_relaxed)) {
        bool active = is_minecraft_active();
        bool prev = g_mc_focused.exchange(active, std::memory_order_relaxed);
        if (prev != active) {
            g_needs_redraw.store(true, std::memory_order_relaxed);
        }
        Sleep(80);
    }
}


HANDLE hConsole = nullptr;

void init_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hConsole, &mode);
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hConsole, mode);

    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);

    SetConsoleTitleW(L"KAIMAN");
}

void cycle_cps_preset() {
    static const double presets[] = { 12.0, 14.0, 16.0, 18.0, 20.0, 21.0, 22.0, 24.0, 26.0 };
    static const int num_presets = sizeof(presets) / sizeof(presets[0]);
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    int current_idx = 5;
    for (int i = 0; i < num_presets; ++i) {
        if (std::abs(g_cfg.cps_min - presets[i]) < 0.2) {
            current_idx = i;
            break;
        }
    }
    int next_idx = (current_idx + 1) % num_presets;
    g_cfg.cps_min = presets[next_idx];
    g_cfg.cps_max = presets[next_idx];
    g_needs_redraw.store(true, std::memory_order_relaxed);
}

void render_ui() {
    ClickerConfig cfg;
    {
        std::lock_guard<std::mutex> lock(g_cfg_mutex);
        cfg = g_cfg;
    }

    bool ingame_active = g_mc_focused.load(std::memory_order_relaxed);
    bool physically_held = physical_button_held();


    const std::string RESET      = "\x1b[0m";
    const std::string BOLD       = "\x1b[1m";
    const std::string P_BORDER   = "\x1b[38;2;168;85;247m";
    const std::string P_TITLE    = "\x1b[38;2;216;180;254m";
    const std::string P_TEXT     = "\x1b[38;2;226;232;240m";
    const std::string P_VAL      = "\x1b[38;2;192;132;252m";
    const std::string P_DARK     = "\x1b[38;2;147;51;234m";
    const std::string S_IDLE     = "\x1b[38;2;244;63;94m";
    const std::string S_ACTIVE   = "\x1b[38;2;74;222;128m";
    const std::string S_CLICKING = "\x1b[38;2;232;121;249m";

    const int TOTAL_WIDTH = 48;
    const int INNER_WIDTH = 44;


    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int console_width = 80;
    if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
        console_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    int indent_spaces = (console_width > TOTAL_WIDTH) ? (console_width - TOTAL_WIDTH) / 2 : 2;
    std::string INDENT(indent_spaces, ' ');

    std::stringstream ss;
    ss << "\x1b[H";


    ss << "\n";
    ss << INDENT << P_BORDER << BOLD << "██╗  ██╗ █████╗ ██╗███╗   ███╗ █████╗ ███╗   ██╗\n";
    ss << INDENT << P_BORDER << BOLD << "██║ ██╔╝██╔══██╗██║████╗ ████║██╔══██╗████╗  ██║\n";
    ss << INDENT << P_DARK   << BOLD << "█████╔╝ ███████║██║██╔████╔██║███████║██╔██╗ ██║\n";
    ss << INDENT << P_DARK   << BOLD << "██╔═██╗ ██╔══██║██║██║╚██╔╝██║██╔══██║██║╚██╗██║\n";
    ss << INDENT << P_BORDER << BOLD << "██║  ██╗██║  ██║██║██║ ╚═╝ ██║██║  ██║██║ ╚████║\n";
    ss << INDENT << P_BORDER << BOLD << "╚═╝  ╚═╝╚═╝  ╚═╝╚═╝╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═══╝\n";
    ss << RESET << "\n";


    auto print_line = [&](const std::string& formatted_content, int visible_len) {
        int pad = INNER_WIDTH - visible_len;
        if (pad < 0) pad = 0;
        ss << INDENT << P_BORDER << "│ " << RESET << formatted_content;
        for (int i = 0; i < pad; ++i) ss << " ";
        ss << P_BORDER << " │\n" << RESET;
    };


    ss << INDENT << P_BORDER << "╭";
    for (int i = 0; i < INNER_WIDTH + 2; ++i) ss << "─";
    ss << "╮\n" << RESET;


    print_line(P_TITLE + BOLD + "[ CONFIGURATION ]" + RESET, 17);

    std::stringstream cps_ss;
    cps_ss << std::fixed << std::setprecision(1) << cfg.cps_min;
    std::string cps_str = cps_ss.str();
    print_line(P_TEXT + "  Normal CPS:  " + P_VAL + BOLD + cps_str + RESET, 15 + (int)cps_str.length());

    print_line("", 0);


    print_line(P_TITLE + BOLD + "[ STATUS ]" + RESET, 10);

    std::string status_vis;
    std::string status_fmt;
    if (!cfg.enabled) {
        status_vis = "IDLE";
        status_fmt = S_IDLE + BOLD + "IDLE" + RESET;
    } else if (physically_held && (!cfg.only_ingame || ingame_active)) {
        status_vis = "CLICKING";
        status_fmt = S_CLICKING + BOLD + "CLICKING" + RESET;
    } else {
        status_vis = "ACTIVE";
        status_fmt = S_ACTIVE + BOLD + "ACTIVE" + RESET;
    }
    print_line(P_TEXT + "  Status:      " + status_fmt, 15 + (int)status_vis.length());

    print_line("", 0);


    print_line(P_TITLE + BOLD + "[ CONTROLS ]" + RESET, 12);
    print_line(P_TITLE + "  [F7]" + P_TEXT + "     Toggle Autoclicker" + RESET, 29);
    print_line(P_TITLE + "  [INSERT]" + P_TEXT + " Change CPS" + RESET, 21);

    print_line("", 0);


    print_line(P_DARK + "  Made by Joshh" + RESET, 15);


    ss << INDENT << P_BORDER << "╰";
    for (int i = 0; i < INNER_WIDTH + 2; ++i) ss << "─";
    ss << "╯\n" << RESET;

    std::string out = ss.str();
    DWORD written = 0;
    WriteConsoleA(hConsole, out.c_str(), (DWORD)out.size(), &written, nullptr);
}

int main() {
    init_console();

    std::cout << "\x1b[2J\x1b[H";
    std::cout.flush();

    std::thread hook_thr(hook_thread_func);
    std::thread clicker_thr(clicker_loop);
    std::thread focus_thr(focus_poll_loop);

    bool prev_f7 = false;
    bool prev_ins = false;

    render_ui();

    while (g_running.load(std::memory_order_relaxed)) {

        bool f7 = key_held(VK_F7);
        if (f7 && !prev_f7) {
            {
                std::lock_guard<std::mutex> lock(g_cfg_mutex);
                g_cfg.enabled = !g_cfg.enabled;
            }
            g_needs_redraw.store(true, std::memory_order_relaxed);
        }
        prev_f7 = f7;


        bool ins = key_held(VK_INSERT);
        if (ins && !prev_ins) {
            cycle_cps_preset();
        }
        prev_ins = ins;


        if (g_needs_redraw.exchange(false, std::memory_order_relaxed)) {
            render_ui();
        }

        Sleep(8);
    }

    g_running.store(false, std::memory_order_relaxed);
    if (g_hook_thread_id) {
        PostThreadMessageW(g_hook_thread_id, WM_QUIT, 0, 0);
    }

    if (hook_thr.joinable()) hook_thr.join();
    if (clicker_thr.joinable()) clicker_thr.join();
    if (focus_thr.joinable()) focus_thr.join();

    return 0;
}
