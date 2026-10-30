#include "tfar_hook.h"

#include "../../extension/src/memscan.h"

#include <xmmintrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ar::voiceplugin::tfar
{
    namespace
    {
        using ProcessFn = void (*)(void* self, float* buffer, int samples);
        using SetErrorFn = void (*)(void* self, float errorLevel);
        using DtorFn = void* (*)(void* self, unsigned int flags);

        // Order matches RadioKind.
        constexpr const char* kClassNames[] = {
            ".?AVPersonalRadioEffect@@",
            ".?AVLongRangeRadioEffect@@",
            ".?AVAirborneRadioEffect@@",
        };
        constexpr int kClasses = 3;
        // TFAR 1.0 ships TFAR_win64.dll; older builds used task_force_radio_win64.dll.
        constexpr const wchar_t* kModuleNames[] = {L"TFAR_win64.dll", L"task_force_radio_win64.dll"};

        struct Class
        {
            uintptr_t vtable = 0;
            ProcessFn process = nullptr;
            SetErrorFn setError = nullptr;
            DtorFn dtor = nullptr;
        };
        Class g_classes[kClasses];

        std::mutex g_installMutex; // generator thread vs Shutdown
        HMODULE g_module = nullptr;
        HookState g_state = HookState::NotLoaded;
        std::string g_detail;
        char g_version[32] = {};

        std::atomic<bool> g_enhanced{true};
        std::atomic<float> g_static{1.f};
        std::atomic<int> g_inFlight{0};
        std::atomic<uint64_t> g_blocks{0};
        std::atomic<float> g_lastLoss{0.f};

        // Our state per TFAR effect object (one per radio per speaker).
        struct Instance
        {
            void* self = nullptr;
            float loss = 0;
            uint64_t lastTick = 0;
            RadioFx fx;
        };
        constexpr int kInstances = 96;
        Instance g_inst[kInstances];
        SRWLOCK g_instLock = SRWLOCK_INIT;

        struct InFlight
        {
            InFlight() { g_inFlight.fetch_add(1, std::memory_order_acq_rel); }
            ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_acq_rel); }
        };

        int ClassOf(void* self)
        {
            const uintptr_t vt = *static_cast<const uintptr_t*>(self);
            for (int i = 0; i < kClasses; ++i)
                if (g_classes[i].vtable == vt)
                    return i;
            return -1;
        }

        // Audio thread creates; the destructor hook (any thread) frees.
        Instance* FindInstance(void* self, RadioKind kind)
        {
            AcquireSRWLockShared(&g_instLock);
            for (Instance& in : g_inst)
                if (in.self == self)
                {
                    ReleaseSRWLockShared(&g_instLock);
                    return &in;
                }
            ReleaseSRWLockShared(&g_instLock);

            AcquireSRWLockExclusive(&g_instLock);
            Instance* slot = nullptr;
            for (Instance& in : g_inst)
            {
                if (in.self == self)
                {
                    ReleaseSRWLockExclusive(&g_instLock);
                    return &in;
                }
                if (!slot || (slot->self && (!in.self || in.lastTick < slot->lastTick)))
                    slot = &in; // free slot, else least recently used
            }
            slot->self = self;
            slot->loss = 0;
            slot->lastTick = GetTickCount64();
            slot->fx.Reset(kind);
            ReleaseSRWLockExclusive(&g_instLock);
            return slot;
        }

        void ForgetInstances()
        {
            AcquireSRWLockExclusive(&g_instLock);
            for (Instance& in : g_inst)
                in.self = nullptr;
            ReleaseSRWLockExclusive(&g_instLock);
        }

        void HookProcess(void* self, float* buffer, int samples)
        {
            InFlight guard;
            const int c = ClassOf(self);
            if (c < 0)
                return;
            if (!g_enhanced.load(std::memory_order_relaxed))
            {
                g_classes[c].process(self, buffer, samples);
                return;
            }
            const RadioKind kind = static_cast<RadioKind>(c);
            Instance* in = FindInstance(self, kind);
            if (in->fx.Kind() != kind)
                in->fx.Reset(kind);
            RadioSettings settings;
            settings.staticLevel = g_static.load(std::memory_order_relaxed);
            const unsigned int csr = _mm_getcsr();
            _mm_setcsr(csr | 0x8040); // flush denormals
            in->fx.Process(buffer, samples, in->loss, settings);
            _mm_setcsr(csr);
            in->lastTick = GetTickCount64();
            g_lastLoss.store(in->loss, std::memory_order_relaxed);
            g_blocks.fetch_add(1, std::memory_order_relaxed);
        }

        void HookSetError(void* self, float errorLevel)
        {
            InFlight guard;
            const int c = ClassOf(self);
            if (c < 0)
                return;
            FindInstance(self, static_cast<RadioKind>(c))->loss = errorLevel;
            g_classes[c].setError(self, errorLevel);
        }

        void* HookDtor(void* self, unsigned int flags)
        {
            InFlight guard;
            const int c = ClassOf(self);
            AcquireSRWLockExclusive(&g_instLock);
            for (Instance& in : g_inst)
                if (in.self == self)
                    in.self = nullptr;
            ReleaseSRWLockExclusive(&g_instLock);
            return c >= 0 ? g_classes[c].dtor(self, flags) : nullptr;
        }

        const uintptr_t kHooks[3] = {
            reinterpret_cast<uintptr_t>(&HookProcess),
            reinterpret_cast<uintptr_t>(&HookSetError),
            reinterpret_cast<uintptr_t>(&HookDtor),
        };

        // Vtables live in read-only .rdata; a write to a module that vanished must not crash.
        bool WriteSlot(uintptr_t addr, uintptr_t value)
        {
            __try
            {
                DWORD old = 0;
                if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), PAGE_READWRITE, &old))
                    return false;
                InterlockedExchange64(reinterpret_cast<volatile LONG64*>(addr), static_cast<LONG64>(value));
                VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), old, &old);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        HMODULE FindTfar()
        {
            for (const wchar_t* name : kModuleNames)
                if (HMODULE m = GetModuleHandleW(name))
                    return m;
            return nullptr;
        }

        void ReadVersion(HMODULE mod)
        {
            g_version[0] = '\0';
            using VersionFn = const char* (*)();
            if (auto fn = reinterpret_cast<VersionFn>(GetProcAddress(mod, "ts3plugin_version")))
                if (const char* v = fn())
                    strncpy_s(g_version, v, _TRUNCATE);
        }

        void Restore()
        {
            for (Class& c : g_classes)
            {
                if (!c.vtable)
                    continue;
                WriteSlot(c.vtable + 0, reinterpret_cast<uintptr_t>(c.process));
                WriteSlot(c.vtable + 8, reinterpret_cast<uintptr_t>(c.setError));
                WriteSlot(c.vtable + 16, reinterpret_cast<uintptr_t>(c.dtor));
            }
        }

        // Called with g_installMutex held.
        bool InstallLocked(HMODULE mod, std::string& why)
        {
            mem::Range text;
            if (!mem::SectionRange(mod, ".text", text))
            {
                why = "no .text section";
                return false;
            }
            Class found[kClasses];
            for (int i = 0; i < kClasses; ++i)
            {
                const uintptr_t vt = mem::FindRttiVtable(mod, kClassNames[i], 0);
                if (!vt)
                {
                    why = std::string("effect class ") + kClassNames[i] + " not found";
                    return false;
                }
                uint64_t slot[3];
                for (int s = 0; s < 3; ++s)
                {
                    if (!mem::SafeRead64(vt + 8 * s, slot[s]))
                    {
                        why = "vtable unreadable";
                        return false;
                    }
                    if (slot[s] == kHooks[s])
                    {
                        why = "already hooked by another copy of this plugin";
                        return false;
                    }
                    if (!text.Contains(static_cast<uintptr_t>(slot[s])))
                    {
                        why = std::string(kClassNames[i]) + " vtable points outside TFAR's code (patched by something else?)";
                        return false;
                    }
                }
                found[i] = {vt, reinterpret_cast<ProcessFn>(slot[0]), reinterpret_cast<SetErrorFn>(slot[1]),
                    reinterpret_cast<DtorFn>(slot[2])};
            }
            // Layout check: all three inherit SimpleRadioEffect::setErrorLeveL, and long range and
            // airborne also inherit its process(). Anything else means the class layout changed.
            if (found[0].setError != found[1].setError || found[1].setError != found[2].setError ||
                found[1].process != found[2].process ||
                reinterpret_cast<uintptr_t>(found[0].process) == reinterpret_cast<uintptr_t>(found[0].setError))
            {
                why = "effect class layout differs from TFAR 1.0 (unknown TFAR build)";
                return false;
            }

            ForgetInstances();
            for (int i = 0; i < kClasses; ++i)
                g_classes[i] = found[i];
            for (const Class& c : g_classes)
            {
                if (!WriteSlot(c.vtable + 0, kHooks[0]) || !WriteSlot(c.vtable + 8, kHooks[1]) ||
                    !WriteSlot(c.vtable + 16, kHooks[2]))
                {
                    Restore();
                    why = "could not write TFAR's vtable";
                    return false;
                }
            }
            return true;
        }

        void Forget()
        {
            for (Class& c : g_classes)
                c = {};
            ForgetInstances();
            g_module = nullptr;
            g_version[0] = '\0';
            g_detail.clear();
        }
    }

    const char* HookStateName(HookState s)
    {
        switch (s)
        {
        case HookState::NotLoaded: return "TFAR plugin not loaded";
        case HookState::Hooked: return "hooked";
        case HookState::Unsupported: return "unsupported TFAR build";
        }
        return "?";
    }

    void SetSettings(bool enhanced, float staticLevel)
    {
        g_enhanced.store(enhanced, std::memory_order_relaxed);
        g_static.store(staticLevel, std::memory_order_relaxed);
    }

    bool InstallInto(HMODULE mod, std::string& why)
    {
        std::lock_guard<std::mutex> lock(g_installMutex);
        Forget();
        g_module = mod;
        ReadVersion(mod);
        const bool ok = InstallLocked(mod, why);
        g_state = ok ? HookState::Hooked : HookState::Unsupported;
        g_detail = ok ? std::string() : why;
        return ok;
    }

    bool Tick()
    {
        std::lock_guard<std::mutex> lock(g_installMutex);
        const HMODULE mod = FindTfar();
        if (!mod)
        {
            if (g_state == HookState::NotLoaded)
                return false;
            Forget(); // TFAR was unloaded; its vtables went with it
            g_state = HookState::NotLoaded;
            return true;
        }
        if (mod == g_module)
        {
            if (g_state == HookState::Unsupported)
                return false;
            uint64_t slot = 0;
            if (g_state == HookState::Hooked && mem::SafeRead64(g_classes[0].vtable, slot) && slot == kHooks[0])
                return false;
        }
        // New (or reloaded, or un-patched) TFAR module.
        Forget();
        g_module = mod;
        ReadVersion(mod);
        std::string why;
        const bool ok = InstallLocked(mod, why);
        g_state = ok ? HookState::Hooked : HookState::Unsupported;
        g_detail = why;
        return true;
    }

    Status GetStatus()
    {
        Status s;
        {
            std::lock_guard<std::mutex> lock(g_installMutex);
            s.state = g_state;
            strncpy_s(s.tfarVersion, g_version, _TRUNCATE);
            s.detail = g_detail;
        }
        s.enhanced = g_enhanced.load(std::memory_order_relaxed);
        s.signalLoss = g_lastLoss.load(std::memory_order_relaxed);
        s.blocks = g_blocks.load(std::memory_order_relaxed);
        const uint64_t now = GetTickCount64();
        AcquireSRWLockShared(&g_instLock);
        for (const Instance& in : g_inst)
            if (in.self && now - in.lastTick < 1000)
                ++s.active;
        ReleaseSRWLockShared(&g_instLock);
        return s;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if (g_state == HookState::Hooked && FindTfar() == g_module)
        {
            Restore();
            // A call that already read our pointer from the vtable may still be entering.
            Sleep(30);
            for (int i = 0; i < 100 && g_inFlight.load() != 0; ++i)
                Sleep(5);
        }
        Forget();
        g_state = HookState::NotLoaded;
    }
}
