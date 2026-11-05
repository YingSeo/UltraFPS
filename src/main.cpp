#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayLayer.hpp>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <mmsystem.h>
#include <GL/gl.h>
#include <intrin.h>
#include <stdlib.h>
#endif

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int interval);
typedef int(__cdecl* PFNNVAPI_INITIALIZE)(void);
typedef HANDLE(WINAPI* PFNCREATEWAITABLETIMEREXW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
typedef BOOL(WINAPI* PFNSETPROCESSINFORMATION)(HANDLE, int, LPVOID, DWORD);

namespace MX350Engine {
    static constexpr double MAX_ALLOWED_FPS = 120.0;
    static constexpr double MIN_FRAME_TIME_SEC = 1.0 / MAX_ALLOWED_FPS;

    static PFNWGLSWAPINTERVALEXTPROC s_wglSwapIntervalEXT = nullptr;
    static HANDLE s_waitableTimer = nullptr;
    static LARGE_INTEGER s_qpcFreq{};
    static LONGLONG s_targetFrameTicks = 0;
    static LONGLONG s_nextFrameTick = 0;
    static LONGLONG s_lastSwapTick = 0;
    static HDC s_lastConfiguredDC = nullptr;
    static bool s_initialized = false;
    static float s_smoothedDt = static_cast<float>(MIN_FRAME_TIME_SEC);
    static int s_frameCounter = 0;

    void initNvidiaControlPanelProfile() {
        _putenv_s("__GL_THREADED_OPTIMIZATIONS", "1");
        _putenv_s("__GL_SYNC_TO_VBLANK", "0");
        _putenv_s("__GL_MAX_FRAMES_ALLOWED", "1");
        _putenv_s("__GL_SHADER_DISK_CACHE", "1");
        _putenv_s("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1");
        _putenv_s("__GL_YIELD", "NOTHING");

        if (HMODULE hNvApi = LoadLibraryA("nvapi64.dll")) {
            using NvQueryInterface_t = void* (__cdecl*)(unsigned int);
            if (auto nvQuery = reinterpret_cast<NvQueryInterface_t>(GetProcAddress(hNvApi, "nvapi_QueryInterface"))) {
                if (auto nvInit = reinterpret_cast<PFNNVAPI_INITIALIZE>(nvQuery(0x0150E828))) {
                    nvInit();
                }
            }
        }

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

    void initSystemKernel() {
        if (s_initialized) return;

        initNvidiaControlPanelProfile();
        timeBeginPeriod(1);

        ULONG heapInfo = 2;
        HeapSetInformation(GetProcessHeap(), HeapCompatibilityInformation, &heapInfo, sizeof(heapInfo));

        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
        SetProcessPriorityBoost(hProcess, FALSE);

        if (HMODULE hKernel = GetModuleHandleA("kernel32.dll")) {
            if (auto pSetProcInfo = reinterpret_cast<PFNSETPROCESSINFORMATION>(GetProcAddress(hKernel, "SetProcessInformation"))) {
                struct { ULONG Version; ULONG ControlMask; ULONG StateMask; } powerState{ 1, 1, 0 };
                pSetProcInfo(hProcess, 4, &powerState, sizeof(powerState));
                ULONG memPriority = 5;
                pSetProcInfo(hProcess, 0, &memPriority, sizeof(memPriority));
            }

            if (auto pCreateTimerEx = reinterpret_cast<PFNCREATEWAITABLETIMEREXW>(GetProcAddress(hKernel, "CreateWaitableTimerExW"))) {
                s_waitableTimer = pCreateTimerEx(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
            }
        }

        if (!s_waitableTimer) {
            s_waitableTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }

        QueryPerformanceFrequency(&s_qpcFreq);
        s_targetFrameTicks = (s_qpcFreq.QuadPart / 120LL) + 1LL;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        s_lastSwapTick = now.QuadPart;
        s_nextFrameTick = now.QuadPart + s_targetFrameTicks;

        s_initialized = true;
    }

    void optimizeOpenGLForHighDetail() {
        HDC currentDC = wglGetCurrentDC();
        if (!currentDC) return;

        if (currentDC != s_lastConfiguredDC) {
            s_lastConfiguredDC = currentDC;
            s_wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(wglGetProcAddress("wglSwapIntervalEXT"));
            if (s_wglSwapIntervalEXT) {
                s_wglSwapIntervalEXT(0);
            }
        }

        glDisable(0x809D);
        glDisable(0x809E);
        glDisable(GL_DITHER);
        glDisable(GL_LINE_SMOOTH);
        glDisable(GL_POLYGON_SMOOTH);
        glDisable(GL_POINT_SMOOTH);

        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
        glHint(GL_GENERATE_MIPMAP_HINT, GL_FASTEST);
        glHint(0x8B8B, GL_FASTEST);
    }

    void enforceStrict120FPSCap() {
        if (!s_initialized) {
            initSystemKernel();
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        LONGLONG minAllowedTick = s_lastSwapTick + s_targetFrameTicks;
        if (s_nextFrameTick < minAllowedTick) {
            s_nextFrameTick = minAllowedTick;
        }

        if (now.QuadPart > s_nextFrameTick) {
            s_nextFrameTick = now.QuadPart;
        }

        LONGLONG targetTick = (s_nextFrameTick > minAllowedTick) ? s_nextFrameTick : minAllowedTick;
        LONGLONG remainingTicks = targetTick - now.QuadPart;

        if (remainingTicks > 0) {
            LONGLONG remaining100ns = (remainingTicks * 10000000LL) / s_qpcFreq.QuadPart;

            if (s_waitableTimer && remaining100ns > 13000LL) {
                LARGE_INTEGER dueTime{};
                dueTime.QuadPart = -(remaining100ns - 9500LL);
                if (SetWaitableTimer(s_waitableTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(s_waitableTimer, 8);
                }
            }

            do {
                _mm_pause();
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < targetTick || (now.QuadPart - s_lastSwapTick) < s_targetFrameTicks);
        }

        s_lastSwapTick = now.QuadPart;
        s_nextFrameTick = targetTick + s_targetFrameTicks;
    }
}
#endif

template <typename GM>
void applySafeGMTarget(GM* gm) {
    if constexpr (requires { gm->m_customFPSTarget = 120.0f; }) {
        gm->m_customFPSTarget = 120.0f;
    }
}

void lockGameSettingsTo120FPS() {
    if (auto* gm = GameManager::sharedState()) {
        gm->setGameVariable("0030", false);
        gm->setGameVariable("0116", true);
        gm->setGameVariable("0082", true);
        applySafeGMTarget(gm);
    }
    if (auto* dir = CCDirector::sharedDirector()) {
        dir->setAnimationInterval(1.0 / 120.0);
    }
}

void optimizeHighDetailNodes(CCNode* root) {
    if (!root) return;
    CCArray* children = root->getChildren();
    if (!children) return;

    unsigned int count = children->count();
    for (unsigned int i = 0; i < count; ++i) {
        auto* node = static_cast<CCNode*>(children->objectAtIndex(i));
        if (!node) continue;

        if (auto* particle = typeinfo_cast<CCParticleSystem*>(node)) {
            if (particle->getTotalParticles() > 45) {
                particle->setEmissionRate(particle->getTotalParticles() * 0.45f);
            }
        }
    }
}

class $modify(MX350EGLView, CCEGLView) {
    void swapBuffers() {
#ifdef GEODE_IS_WINDOWS
        MX350Engine::optimizeOpenGLForHighDetail();
        MX350Engine::enforceStrict120FPSCap();
#endif
        CCEGLView::swapBuffers();
    }
};

class $modify(MX350Scheduler, CCScheduler) {
    void update(float dt) {
        constexpr float TARGET_DT = static_cast<float>(1.0 / 120.0);

        if (auto* dir = CCDirector::sharedDirector()) {
            if (dir->getAnimationInterval() < (1.0 / 120.0)) {
                dir->setAnimationInterval(1.0 / 120.0);
            }
        }

        if (dt <= 0.0f || dt > 0.0166667f) {
            dt = TARGET_DT;
        } else {
#ifdef GEODE_IS_WINDOWS
            MX350Engine::s_smoothedDt = (MX350Engine::s_smoothedDt * 0.85f) + (dt * 0.15f);
            dt = MX350Engine::s_smoothedDt;
#endif
        }

#ifdef GEODE_IS_WINDOWS
        if (++MX350Engine::s_frameCounter >= 60) {
            MX350Engine::s_frameCounter = 0;
            if (auto* pl = PlayLayer::get()) {
                optimizeHighDetailNodes(pl);
            }
        }
#endif

        CCScheduler::update(dt);
    }
};

class $modify(MX350PlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        lockGameSettingsTo120FPS();

        if (auto* sf = CCSpriteFrameCache::sharedSpriteFrameCache()) {
            sf->removeUnusedSpriteFrames();
        }
        if (auto* tc = CCTextureCache::sharedTextureCache()) {
            tc->removeUnusedTextures();
        }

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        lockGameSettingsTo120FPS();
        optimizeHighDetailNodes(this);
        return true;
    }

    void onQuit() {
        PlayLayer::onQuit();
        if (auto* tc = CCTextureCache::sharedTextureCache()) {
            tc->removeUnusedTextures();
        }
    }
};

$on_mod(Loaded) {
#ifdef GEODE_IS_WINDOWS
    MX350Engine::initSystemKernel();
#endif
    Loader::get()->queueInMainThread([]() {
        lockGameSettingsTo120FPS();
    });
}
