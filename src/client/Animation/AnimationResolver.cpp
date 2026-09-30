// Extended playable-animation resolution for modern M2 models.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "../../ExtensionApi.hpp"

#include "game/M2Animation.hpp"
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

    bool ModelHasSequenceGuarded(void* model, uint32_t animationId) noexcept
    {
        __try
        {
            return animation::ModelHasSequence(model, animationId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int ResolveExtended(void* unit, int animationId, void* model) noexcept
    {
        if (animationId <= animation::kCurrentOrNone) return -1;

        __try
        {
            if (!model) model = animation::UnitModel(unit);
            if (animation::ModelHasSequence(model, static_cast<uint32_t>(animationId)))
                return animationId;

            int current = animationId;
            for (int depth = 0; depth < 16; ++depth)
            {
                const animation::AnimationDataRow* row = animation::Lookup(
                    static_cast<uint32_t>(current));
                if (!row || static_cast<int>(row->fallback) == current) break;

                current = static_cast<int>(row->fallback);
                if (current >= animation::kCurrentOrNone &&
                    animation::ModelHasSequence(model, static_cast<uint32_t>(current)))
                    return current;
                if (current < animation::kCurrentOrNone) return current;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return -1;
    }

    bool __fastcall HasPlayable(void* model, void*, uint32_t animationId)
    {
        if (animationId <= static_cast<uint32_t>(animation::kCurrentOrNone))
            return g_originalHasPlayable(model, animationId);
        return ModelHasSequenceGuarded(model, animationId);
    }

    int __fastcall FindPlayable(void* model, void*, uint32_t animationId)
    {
        if (animationId <= static_cast<uint32_t>(animation::kCurrentOrNone))
            return g_originalFindPlayable(model, animationId);
        return ModelHasSequenceGuarded(model, animationId)
            ? static_cast<int>(animationId) : -1;
    }

    int __fastcall ResolveAnimation(void* unit, void*, int animationId, void* model)
    {
        int resolved = animationId > animation::kCurrentOrNone
            ? ResolveExtended(unit, animationId, model) : -1;
        if (resolved < 0) resolved = g_originalResolve(unit, animationId, model);

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
        WLOG_INFO("m2-animation: extended animation IDs >= %d enabled",
                  animation::kCurrentOrNone + 1);
        return true;
    }
}
