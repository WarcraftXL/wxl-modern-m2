// Extended animation resolution: makes an animation id past the engine's own ceiling playable when
// the model carries it.
//
// The engine resolves an animation in two steps -- does this model have the sequence, and if not
// what does the table say to play instead -- and both steps refuse an id above a fixed bound. The
// bound is not a statement about content: the fallback walk marks visited ids in a stack array
// indexed by the id, so the ceiling is that buffer's size. A modern model carrying a sequence the
// table has no row for is therefore invisible to the whole path, no matter how well it loaded.
//
// This detours the three entries and answers for the ids above the bound only. Below it, the call
// goes straight through -- which is also why it installs unconditionally: on stock content every
// detour here is a comparison and a jump to the original.
//
// Ids above the bound are never handed back to the native entries, since that array is what the
// bound protects. When the table's fallback chain leads back down into stock territory, the id it
// landed on is what goes to the native resolver, so variation selection stays the engine's job.
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

#include "../../ExtensionApi.hpp"

#include "game/M2Animation.hpp"
#include "game/Unit.hpp"
#include "wxl/M2AnimationApi.h"

#include <windows.h>

#include <cstdint>

namespace
{
    namespace animation = wxl::game::m2animation;

    using HasPlayableFn = bool(__thiscall*)(void* model, uint32_t animationId);
    using FindPlayableFn = int(__thiscall*)(void* model, uint32_t animationId);
    using ResolveAnimationFn = int(__thiscall*)(void* unit, int animationId, void* model);

    HasPlayableFn g_originalHasPlayable = nullptr;
    FindPlayableFn g_originalFindPlayable = nullptr;
    ResolveAnimationFn g_originalResolve = nullptr;
    WXL_M2AnimationResolveOverrideFn g_override = nullptr;

    /// One line per session is enough to tell a bad model from a bad binding; the fault is silent
    /// after that rather than filling the log from an animating frame.
    bool g_faultReported = false;

    /// What every entry here returns when it has no animation to offer.
    constexpr int kUnresolved = -1;

    /// Enough to cross any chain the table actually holds, and short enough that a cycle the
    /// self-reference check misses still ends.
    constexpr int kMaxFallbackSteps = 16;

    /// Whether an id is one of ours to answer for. A negative id is not: it is the caller's own
    /// "nothing", and widening it would turn it into a very large one.
    bool IsExtended(int animationId)
    {
        return animationId > 0 && static_cast<uint32_t>(animationId) > animation::kLastStockId;
    }

    /**
     * @brief Asks the model whether it carries a sequence, under the module's crash-safe contract.
     *
     * The query walks arrays whose counts and offsets came out of the model file, so a malformed one
     * faults here rather than anywhere the client could have caught it. A fault means this model
     * cannot answer, which is the same outcome as not having the sequence.
     */
    bool ModelHasSequenceGuarded(void* model, uint32_t animationId) noexcept
    {
        __try
        {
            return animation::ModelHasSequence(model, animationId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_faultReported)
            {
                g_faultReported = true;
                WLOG_WARN("m2-animation: model faulted answering for animation %u", animationId);
            }
            return false;
        }
    }

    /**
     * @brief Finds an extended animation the model can play, following the table's fallbacks.
     * @param model          model to play it on.
     * @param animationId    extended id asked for.
     * @param stockFallback  receives the stock id the chain dropped to, when it left extended
     *                       territory without a playable sequence; untouched otherwise.
     * @return an extended id the model carries, or kUnresolved.
     */
    int ResolveExtended(void* model, int animationId, int* stockFallback)
    {
        if (!model) return kUnresolved;
        if (ModelHasSequenceGuarded(model, static_cast<uint32_t>(animationId)))
            return animationId;

        uint32_t current = static_cast<uint32_t>(animationId);
        for (int step = 0; step < kMaxFallbackSteps; ++step)
        {
            const animation::AnimationRow* row = animation::Lookup(current);
            if (!row || row->fallback == current) break;

            current = row->fallback;
            if (current <= animation::kLastStockId)
            {
                *stockFallback = static_cast<int>(current);
                break;
            }
            if (ModelHasSequenceGuarded(model, current))
                return static_cast<int>(current);
        }
        return kUnresolved;
    }

    bool __fastcall HasPlayable(void* model, void*, uint32_t animationId)
    {
        if (!IsExtended(static_cast<int>(animationId)))
            return g_originalHasPlayable(model, animationId);
        return ModelHasSequenceGuarded(model, animationId);
    }

    int __fastcall FindPlayable(void* model, void*, uint32_t animationId)
    {
        if (!IsExtended(static_cast<int>(animationId)))
            return g_originalFindPlayable(model, animationId);
        return ModelHasSequenceGuarded(model, animationId)
            ? static_cast<int>(animationId) : kUnresolved;
    }

    int __fastcall ResolveAnimation(void* unit, void*, int animationId, void* model)
    {
        int resolved = kUnresolved;
        if (IsExtended(animationId))
        {
            int stockFallback = kUnresolved;
            resolved = ResolveExtended(model ? model : wxl::game::unit::Model(unit), animationId,
                                       &stockFallback);
            if (resolved == kUnresolved && stockFallback != kUnresolved)
                resolved = g_originalResolve(unit, stockFallback, model);
        }
        else
        {
            resolved = g_originalResolve(unit, animationId, model);
        }

        if (g_override)
        {
            const int forced = g_override(unit, animationId, model, resolved);
            if (forced >= 0) return forced;
        }
        return resolved;
    }

    void __cdecl SetResolveOverride(WXL_M2AnimationResolveOverrideFn callback)
    {
        g_override = callback;
    }

    WXL_M2AnimationApi g_animationApi = {
        sizeof(WXL_M2AnimationApi),
        WXL_M2_ANIMATION_API_VERSION,
        &SetResolveOverride,
    };

    bool Attach(const char* pointName, void* detour, void** original)
    {
        return wxl_modern_m2::g_api->HookAttachByName(
            pointName, detour, original, WXL_HOOK_DEFAULT_PRIORITY) != 0;
    }
}

namespace wxl_modern_m2
{
    bool InstallExtendedAnimations()
    {
        bool ok = true;
        ok &= Attach("M2.HasPlayableAnimation", reinterpret_cast<void*>(&HasPlayable),
                     reinterpret_cast<void**>(&g_originalHasPlayable));
        ok &= Attach("M2.FindPlayableAnimation", reinterpret_cast<void*>(&FindPlayable),
                     reinterpret_cast<void**>(&g_originalFindPlayable));
        ok &= Attach("Unit.ResolveModelAnimation", reinterpret_cast<void*>(&ResolveAnimation),
                     reinterpret_cast<void**>(&g_originalResolve));
        if (!ok) return false;

        g_api->PublishInterface("wxl.m2-animation", WXL_M2_ANIMATION_API_VERSION,
                                &g_animationApi);
        WLOG_INFO("m2-animation: animation ids above %u resolved against the model",
                  animation::kLastStockId);
        return true;
    }
}
