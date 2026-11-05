#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCParticleSystem.hpp>

#ifdef GEODE_IS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <GL/gl.h>
#include <intrin.h>
#include <stdlib.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Cấu trúc ưu tiên RAM vật lý (chống tràn Pagefile trên máy 8GB RAM)
typedef struct _MOD_MEMORY_PRIORITY_INFO {
    ULONG MemoryPriority;
} MOD_MEMORY_PRIORITY_INFO;
#ifndef MemoryPriorityInformation
#define MemoryPriorityInformation static_cast<PROCESS_INFORMATION_CLASS>(0)
#endif
#endif

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
// Xuất cờ hiệu năng cao cho Driver Nvidia Optimus & AMD
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int interval);
typedef int(__cdecl* PFNNVAPI_INITIALIZE)(void);

namespace MX350Engine {
    static PFNWGLSWAPINTERVALEXTPROC s_wglSwapIntervalEXT = nullptr;
    static HANDLE s_waitableTimer = nullptr;
    static LARGE_INTEGER s_qpcFreq{};
    static LONGLONG s_targetFrameTicks = 0;
    static LONGLONG s_nextFrameTick = 0;
    static HDC s_lastConfiguredDC = nullptr;
    static bool s_initialized = false;

    static constexpr double TARGET_FPS = 120.0;
    static constexpr double TARGET_FRAME_TIME = 1.0 / TARGET_FPS; // 0.008333333333333333s

    // 1. Kích hoạt trực tiếp Driver Nvidia (nvapi64.dll) & cấu hình Control Panel
    void initNvidiaDriverAndRegistry() {
        // Thiết lập biến môi trường tầng sâu của Driver Nvidia OpenGL
        _putenv_s("__GL_THREADED_OPTIMIZATIONS", "1");       // Bật xử lý đa luồng trên GPU
        _putenv_s("__GL_SYNC_TO_VBLANK", "0");               // Gỡ khóa V-Sync 60Hz của màn hình
        _putenv_s("__GL_MAX_FRAMES_ALLOWED", "1");           // Ultra Low Latency (chống trễ thao tác nhảy)
        _putenv_s("__GL_SHADER_DISK_CACHE", "1");            // Lưu bộ đệm Shader chống giật
        _putenv_s("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1");
        _putenv_s("__GL_YIELD", "NOTHING");                  // Giữ xung nhịp MX350 không bị tụt giữa các frame

        // Nạp động nvapi64.dll để đánh thức card rời GeForce MX350 ngay trong tiến trình
        HMODULE hNvApi = LoadLibraryA("nvapi64.dll");
        if (hNvApi) {
            using NvQueryInterface_t = void* (__cdecl*)(unsigned int);
            auto nvQuery = reinterpret_cast<NvQueryInterface_t>(GetProcAddress(hNvApi, "nvapi_QueryInterface"));
            if (nvQuery) {
                // 0x0150E828 = ID hàm NvAPI_Initialize
                auto nvInit = reinterpret_cast<PFNNVAPI_INITIALIZE>(nvQuery(0x0150E828));
                if (nvInit) {
                    nvInit();
                }
            }
        }

        // Ghi vào Windows DirectX UserGpuPreferences để ép Windows 11 gán MX350 (High Performance)
        char exePath[MAX_PATH]{};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
            HKEY hKey = nullptr;
            const char* regPath = "Software\\Microsoft\\DirectX\\UserGpuPreferences";
            if (RegCreateKeyExA(HKEY_CURRENT_USER, regPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
                const char* gpuPref = "GpuPreference=2;SwapEffectUpgradeEnable=1;";
                RegSetValueExA(hKey, exePath, 0, REG_SZ, reinterpret_cast<const BYTE*>(gpuPref), static_cast<DWORD>(strlen(gpuPref) + 1));
                RegCloseKey(hKey);
            }
        }
    }

    // 2. Tối ưu hóa Kernel Windows 11, CPU & Quản lý RAM 8GB
    void initWindowsKernelAndMemory() {
        if (s_initialized) return;

        initNvidiaDriverAndRegistry();

        // Hạ chu kỳ ngắt hệ thống xuống 1ms
        timeBeginPeriod(1);

        // Bật Low-Fragmentation Heap (LFH) chống phân mảnh RAM và chống đơ game khi chơi màn nhiều Object
        ULONG heapInfo = 2;
        HeapSetInformation(GetProcessHeap(), HeapCompatibilityInformation, &heapInfo, sizeof(heapInfo));

        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();

        // Nâng mức ưu tiên tiến trình và luồng chính
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
        SetProcessPriorityBoost(hProcess, FALSE);

        // Tắt hoàn toàn Power Throttling của Windows 11 (Ngăn tự động bóp xung CPU/GPU)
#ifdef PROCESS_POWER_THROTTLING_CURRENT_VERSION
        PROCESS_POWER_THROTTLING_STATE powerThrottling{};
        powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        powerThrottling.StateMask = 0;
        SetProcessInformation(hProcess, ProcessPowerThrottling, &powerThrottling, sizeof(powerThrottling));
#endif

        // Giữ dữ liệu Geometry Dash luôn nằm trên RAM vật lý (Memory Priority = 5), không bị đẩy xuống ổ cứng
        MOD_MEMORY_PRIORITY_INFO memPriority{};
        memPriority.MemoryPriority = 5; // MEMORY_PRIORITY_NORMAL (mức cao nhất cho ứng dụng người dùng)
        SetProcessInformation(hProcess, MemoryPriorityInformation, &memPriority, sizeof(memPriority));

        // Khởi tạo High-Resolution Waitable Timer của Windows 10/11 (Độ chính xác 0.5ms)
        s_waitableTimer = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );
        if (!s_waitableTimer) {
            s_waitableTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }

        // Khởi tạo bộ đếm phần cứng QPC
        QueryPerformanceFrequency(&s_qpcFreq);
        s_targetFrameTicks = static_cast<LONGLONG>(static_cast<double>(s_qpcFreq.QuadPart) / TARGET_FPS);

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        s_nextFrameTick = now.QuadPart + s_targetFrameTicks;

        s_initialized = true;
    }

    // 3. Cấu hình OpenGL thông minh (Chỉ gọi 1 lần khi tạo ngữ cảnh, không gọi lặp gây nghẽn)
    void ensureOpenGLContextOptimized() {
        HDC currentDC = wglGetCurrentDC();
        if (!currentDC || currentDC == s_lastConfiguredDC) {
            return;
        }
        s_lastConfiguredDC = currentDC;

        // Tắt khử răng cưa đa mẫu và Dither để tăng tốc độ dựng hình các màn nhiều Glow
        glDisable(0x809D); // GL_MULTISAMPLE
        glDisable(GL_DITHER);
        glDisable(GL_LINE_SMOOTH);
        glDisable(GL_POLYGON_SMOOTH);

        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
        glHint(GL_GENERATE_MIPMAP_HINT, GL_FASTEST);

        // Mở khóa giới hạn 60Hz của màn hình ở tầng Driver OpenGL
        s_wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(wglGetProcAddress("wglSwapIntervalEXT"));
        if (s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT(0);
        }
    }

    // 4. Bộ khóa cứng 120.0 FPS Không Trôi Nhịp (Zero-Drift Frame Limiter)
    void lockExact120FPS() {
        if (!s_initialized) {
            initWindowsKernelAndMemory();
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        // Nếu bị trễ quá 2 khung hình (ví dụ lúc vừa chuyển cảnh), đồng bộ lại mốc thời gian để chống đơ/tua nhanh
        if (now.QuadPart > s_nextFrameTick + (s_targetFrameTicks * 2)) {
            s_nextFrameTick = now.QuadPart;
        }

        LONGLONG remainingTicks = s_nextFrameTick - now.QuadPart;

        if (remainingTicks > 0) {
            // Tính số phần trăm nano-giây (100ns units) còn lại
            LONGLONG remaining100ns = (remainingTicks * 10000000LL) / s_qpcFreq.QuadPart;

            // Nếu thời gian chờ > 1.2ms, dùng High-Resolution Waitable Timer để giải phóng tải cho CPU & MX350
            if (s_waitableTimer && remaining100ns > 12000LL) {
                LARGE_INTEGER dueTime{};
                // Trừ hao 0.9ms (9000 * 100ns) cuối cùng để nhường cho vòng lặp _mm_pause() chốt chính xác
                dueTime.QuadPart = -(remaining100ns - 9000LL);
                if (SetWaitableTimer(s_waitableTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(s_waitableTimer, 8);
                }
            }

            // Spin-wait nhẹ bằng chỉ thị _mm_pause() trong < 0.9ms cuối để khóa đúng 8.3333ms (120.0 FPS)
            do {
                _mm_pause();
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < s_nextFrameTick);
        }

        // Cộng dồn mốc thời gian cố định để tuyệt đối không bị trôi FPS (120.0 FPS phẳng)
        s_nextFrameTick += s_targetFrameTicks;
    }
}
#endif

// Đồng bộ trạng thái 120 FPS với hệ thống nội bộ của Geometry Dash 2.2081
void syncGameEngine120FPS() {
    if (auto* gm = GameManager::sharedState()) {
        gm->setGameVariable("0030", false); // Tắt V-Sync nội bộ của game
        gm->setGameVariable("0116", true);  // Bật Custom FPS của GD 2.2081
        gm->m_customFPSTarget = 120.0f;     // Đặt đích 120 FPS
    }
    if (auto* app = CCApplication::sharedApplication()) {
        app->toggleVerticalSync(false);
    }
    if (auto* dir = CCDirector::sharedDirector()) {
        dir->setAnimationInterval(1.0 / 120.0);
    }
}

// 1. Hook CCEGLView: Thực thi khóa cứng 120.0 FPS và tối ưu ngữ cảnh OpenGL
class $modify(MX350EGLView, CCEGLView) {
    void swapBuffers() {
#ifdef GEODE_IS_WINDOWS
        MX350Engine::ensureOpenGLContextOptimized();
        MX350Engine::lockExact120FPS();
#endif
        CCEGLView::swapBuffers();
    }
};

// 2. Hook CCScheduler: Chống đơ game (Anti-Freeze) và ổn định vật lý ở 120Hz
class $modify(MX350Scheduler, CCScheduler) {
    void update(float dt) {
        // Chặn hiện tượng vọt deltaTime (Death Spiral) khi nạp âm thanh hoặc chuyển cảnh nặng
        if (dt > 0.025f || dt <= 0.0f) {
            dt = static_cast<float>(1.0 / 120.0);
        }
        CCScheduler::update(dt);
    }
};

// 3. Hook CCParticleSystem (Đã sửa đúng lớp của Geode 2.2081): Tối ưu màn nhiều hiệu ứng hạt
class $modify(MX350ParticleOptimizer, CCParticleSystem) {
    void update(float dt) {
        // Giới hạn số lượng hạt đang vẽ trên mỗi hệ thống hạt để chống sụt FPS ở các đoạn Bossfight/Glow dày
        if (m_uParticleCount > 150) {
            m_uParticleCount = 150;
        }
        CCParticleSystem::update(dt);
    }
};

// 4. Hook PlayLayer: Quản lý bộ nhớ thông minh cho máy 8GB RAM mà không gây giật đầu màn
class $modify(MX350PlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        syncGameEngine120FPS();

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        syncGameEngine120FPS();
        return true;
    }

    void onQuit() {
        PlayLayer::onQuit();
        // Chỉ dọn dẹp Texture rác sau khi thoát màn chơi để giải phóng RAM cho màn tiếp theo
        if (auto* tc = CCTextureCache::sharedTextureCache()) {
            tc->removeUnusedTextures();
        }
    }
};

// 5. Tự động kích hoạt toàn bộ hệ thống ngay khi vào game
$on_mod(Loaded) {
#ifdef GEODE_IS_WINDOWS
    MX350Engine::initWindowsKernelAndMemory();
#endif
    Loader::get()->queueInMainThread([]() {
        syncGameEngine120FPS();
    });
}
