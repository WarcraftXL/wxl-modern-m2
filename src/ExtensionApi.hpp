// wxl-m2: the extension-wide service table pointer and hook-install convenience, shared by every
// translation unit in this DLL.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "wxl/FdidApi.h"
#include "wxl/M2ArenaApi.h"
#include "wxl/M2DrawApi.h"
#include "wxl/PluginApi.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

/// See wxl-adt's ExtensionApi.hpp for the reasoning behind every pattern here: the core hands this
/// pointer to WXL_Load once and it lives for the process lifetime, so every detour installed later
/// reaches it through here rather than threading an `api` parameter through the call chain.
namespace wxl_m2
{
    extern const WXL_Api* g_api;

    /// True builds the whole native M2 pipeline in; false is the single point that would leave the
    /// client on its stock 3.3.5 M2 reader (ex wxl::features::modernM2Support).
    inline constexpr bool kEnabled = true;

    // --- wxl-db2's FileDataID resolver, fetched lazily (extensions load alphabetically; wxl-db2
    // loads before wxl-m2, so this always resolves once anything actually asks for a path). ----------
    extern const WXL_FdidApi* g_fdid;

    inline const WXL_FdidApi* Fdid()
    {
        if (!g_fdid)
            g_fdid = static_cast<const WXL_FdidApi*>(g_api->GetInterface("wxl.fdid", WXL_FDID_API_VERSION));
        return g_fdid;
    }

    inline const char* ResolveTexture(uint32_t fileDataId)
    {
        const WXL_FdidApi* fdid = Fdid();
        return fdid ? fdid->ResolveTexture(fileDataId) : nullptr;
    }

    // --- the core-owned large-M2 arena, published (Boot phase, before any extension loads) as
    // "wxl.m2arena" -- always present by the time wxl-m2's own WXL_Load runs, but fetched through the
    // same lazy accessor as every other cross-binary service for one uniform pattern. -----------------
    extern const WXL_M2ArenaApi* g_arena;

    inline const WXL_M2ArenaApi* Arena()
    {
        if (!g_arena)
            g_arena = static_cast<const WXL_M2ArenaApi*>(g_api->GetInterface("wxl.m2arena", WXL_M2ARENA_API_VERSION));
        return g_arena;
    }

    /**
     * @brief Typed detour install over WXL_Api::HookAttach: detour and original share one function
     *        type, deduced, so wiring a hook to the wrong original no longer compiles. Mirrors
     *        wxl::hook::Install's own convenience overload, which this extension cannot link against.
     */
    template <class Fn>
    inline int HookAttach(const char* name, uintptr_t target, Fn* detour, Fn** original,
                          int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttach(name, target, reinterpret_cast<void*>(detour),
                                 reinterpret_cast<void**>(original), priority);
    }

    // Per-file installers, called from WXL_Load in Module.cpp.
    bool InstallM2CompatBones();       // BonePalette.cpp (gated)
    bool InstallEmitterBlend();        // EmitterBlend.cpp (gated)
    bool InstallM2PerFrameUpdate();    // PerFrameUpdate.cpp (gated)
    bool InstallM2SceneHitTestSort();  // HitTestSort.cpp (gated)
    bool InstallM2Draw();              // M2Draw.cpp (unconditional -- owns the DIP vtable slot)
    bool InstallM2SetupBatchAlpha();   // SetupMaterial.cpp (gated)
    bool InstallAnimUnwrap();          // AnimUnwrap.cpp (gated)
    bool InstallM2CompatLoader();      // CompatLoader.cpp (gated)
    bool InstallM2Native();            // NativeLoad.cpp (gated)
    bool InstallM2Memory();            // Memory.cpp (gated)
    bool InstallModernM2();            // ModernM2.cpp (gated)

    /// Env-var + this module's own config file (ex wxl::config::*, which reads WarcraftXL.cfg --
    /// core-only, an extension has no such object file to link). Same "KEY=value", '#'/';' comment,
    /// trimmed format as WarcraftXL.cfg; env var always wins, matching the core's own precedence.
    inline bool ConfigTruthy(const char* raw, bool fallback)
    {
        if (!raw || !raw[0]) return fallback;
        switch (raw[0])
        {
        case '0': case 'n': case 'N': case 'f': case 'F': return false;
        default: return true;
        }
    }

    inline const std::unordered_map<std::string, std::string>& ConfigFile()
    {
        static const std::unordered_map<std::string, std::string> entries = [] {
            std::unordered_map<std::string, std::string> map;
            FILE* f = nullptr;
            if (fopen_s(&f, "Extensions\\wxl-m2\\wxl-m2.cfg", "rb") != 0 || !f) return map;
            char line[512];
            while (fgets(line, sizeof line, f))
            {
                char* text = line;
                while (*text == ' ' || *text == '\t') ++text;
                if (*text == '#' || *text == ';' || *text == '\0') continue;
                char* eq = std::strchr(text, '=');
                if (!eq) continue;
                char* keyEnd = eq;
                while (keyEnd > text && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) --keyEnd;
                char* value = eq + 1;
                while (*value == ' ' || *value == '\t') ++value;
                char* valueEnd = value + std::strlen(value);
                while (valueEnd > value && (valueEnd[-1] == '\n' || valueEnd[-1] == '\r'
                                         || valueEnd[-1] == ' '  || valueEnd[-1] == '\t')) --valueEnd;
                if (keyEnd > text)
                    map.emplace(std::string(text, keyEnd), std::string(value, valueEnd));
            }
            fclose(f);
            return map;
        }();
        return entries;
    }

    inline bool ConfigRaw(const char* name, char* buf, size_t cap)
    {
        if (!name || !buf || !cap) return false;
        size_t written = 0;
        if (getenv_s(&written, buf, cap, name) == 0 && written > 0) return true;

        const auto& cfg = ConfigFile();
        const auto it = cfg.find(name);
        if (it == cfg.end() || it->second.empty() || it->second.size() + 1 > cap) return false;
        std::memcpy(buf, it->second.c_str(), it->second.size() + 1);
        return true;
    }

    /// Feature toggle matching ex wxl::config::Flag: an env var (falsy value disables) plus a
    /// .disable sentinel FILE (checked directly, independent of the .cfg mechanism), default ON.
    inline bool ConfigFlag(const char* envName, const char* disableFile)
    {
        char value[16] = {};
        if (ConfigRaw(envName, value, sizeof value) && !ConfigTruthy(value, true))
            return false;
        if (disableFile && GetFileAttributesA(disableFile) != INVALID_FILE_ATTRIBUTES)
            return false;
        return true;
    }

    inline uint64_t ConfigU64(const char* name, uint64_t fallback, uint64_t minValue, uint64_t maxValue)
    {
        char value[32] = {};
        if (!ConfigRaw(name, value, sizeof value)) return fallback;
        char* end = nullptr;
        const uint64_t parsed = std::strtoull(value, &end, 10);
        if (end == value) return fallback;
        if (parsed < minValue) return minValue;
        if (parsed > maxValue) return maxValue;
        return parsed;
    }

    inline uint32_t ConfigU32(const char* name, uint32_t fallback, uint32_t minValue, uint32_t maxValue)
    {
        return static_cast<uint32_t>(ConfigU64(name, fallback, minValue, maxValue));
    }

    /// ex wxl::config::BytesMbKb: an MB env var, then a KB one, then a default; a candidate outside
    /// [minKb, maxKb] is rejected and the next source tried.
    inline uint32_t ConfigBytesMbKb(const char* envMb, const char* envKb, uint32_t defBytes,
                                    uint32_t minKb, uint32_t maxKb)
    {
        char value[32] = {};
        if (ConfigRaw(envMb, value, sizeof value))
        {
            char* end = nullptr;
            const uint64_t mb = std::strtoull(value, &end, 10);
            const uint64_t kb = mb * 1024ull;
            if (end != value && kb >= minKb && kb <= maxKb)
                return static_cast<uint32_t>(kb * 1024ull);
        }
        if (ConfigRaw(envKb, value, sizeof value))
        {
            char* end = nullptr;
            const uint64_t kb = std::strtoull(value, &end, 10);
            if (end != value && kb >= minKb && kb <= maxKb)
                return static_cast<uint32_t>(kb * 1024ull);
        }
        return defBytes;
    }
}

// common/Log.hpp's WLOG_* macros need common/Log.cpp linked in, which is core/host/patcher-only (see
// its own doc comment) -- an extension has no such object file, so these route the same call-site
// syntax through WXL_Api::Log instead.
#define WLOG_TRACE(...) ::wxl_m2::g_api->Log(WXL_LOG_TRACE, "wxl-m2", __VA_ARGS__)
#define WLOG_DEBUG(...) ::wxl_m2::g_api->Log(WXL_LOG_DEBUG, "wxl-m2", __VA_ARGS__)
#define WLOG_INFO(...)  ::wxl_m2::g_api->Log(WXL_LOG_INFO,  "wxl-m2", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_m2::g_api->Log(WXL_LOG_WARN,  "wxl-m2", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_m2::g_api->Log(WXL_LOG_ERROR, "wxl-m2", __VA_ARGS__)
