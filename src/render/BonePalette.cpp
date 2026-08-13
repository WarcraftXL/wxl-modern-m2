// M2 bone compatibility: post-fill bone-palette event and guards for co-instanced draw paths whose
// native bone uploads can overrun the fixed vertex-shader constant range.
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

#include "../ExtensionApi.hpp"
#include "BoneOverflowGuard.hpp"
#include "ShadowSpace.hpp"

#include "engine/events/Event.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"

#include "offsets/engine/Gx.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace
{
    namespace ev    = wxl::events;
    namespace m2    = wxl::offsets::game::m2;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace guard = wxl_modern_m2;
    m2::M2_BuildBonePaletteFn     g_origBuildBonePalette     = nullptr;
    m2::M2_RenderBatchShadowMapFn g_origRenderBatchShadowMap = nullptr;
    std::atomic<uint32_t>         g_shadowBoneOverflowSkips{ 0 };

    using DrawBatchDoodadFn = void (__fastcall*)(void* ctx, void* edx, void* elements, void* indices);
    DrawBatchDoodadFn g_origDrawBatchDoodad = nullptr;
    std::atomic<uint32_t> g_doodadBoneOverflowSkips{ 0 };

    /**
     * @brief Detours bone-palette build, emitting OnBuildBonePalette after the engine fills the buffer.
     *
     * Called from two sites per collection M2 per frame:
     *   (a) the attached-model update path, inside kM2PerFrameUpdate of the parent character.
     *   (b) The outer scene-traversal loop (0x821B4E), which runs AFTER the parent's PerFrameUpdate.
     *
     * Site (b) overwrites any bone-palette modifications that OnM2PerFrameUpdate subscribers made,
     * reverting the collection M2 to its bind pose every frame. By hooking POST-order here,
     * subscribers can re-apply their modifications immediately after the engine's fill -- guaranteed
     * to be the last write before the GPU upload regardless of scene-list ordering.
     *
     * Calling convention: fastcall, ecx = renderCtx, 5 stack args, ret 0x14 (callee-cleanup).
     */
    void __fastcall hkBuildBonePalette(void* renderCtx, void* edx,
        void* sa1, void* sa2, void* sa3, uint32_t sa4, uint32_t sa5)
    {
        g_origBuildBonePalette(renderCtx, edx, sa1, sa2, sa3, sa4, sa5);
        ev::BuildBonePaletteArgs a{ renderCtx };
        wxl_modern_m2::g_api->Emit(uint32_t(ev::Event::OnBuildBonePalette), &a);
    }

    /**
     * @brief Rejects an unsafe ground-shadow batch before native rendering can corrupt Gx state.
     *
     * This detour is the ONLY one the client's real M2 ground-shadow draw can carry (MinHook
     * rejects a second on the same target), so the shadow bone probe rides it from here too.
     *
     * The native function's own bone-copy loop (c31-based, 3 registers/bone) is unbounded across the
     * whole co-instance run -- boneCount * coInstanceCount can exceed the 75-bone VS-constant budget
     * even when boneCount alone is small, overflowing past c255 into the device's own vertex-stream
     * slot cache. That overflow is a confirmed, disasm-verified crash: it corrupts a slot record's
     * "count" dword with a bone-matrix float, which FUN_006844c0 later reads as an array index and
     * faults on a wild address (see corpus/re_comprehension/335/m2_instance_0x184_gx_cache.md §14 for
     * the original trace, and the register-level confirmation recorded in this session's own crash
     * triage). Mutating the run and issuing several calls is not safe: drawIndex selects the run
     * record, not an arbitrary instance offset. Skip the affected shadow batch and leave the list
     * intact.
     */
    void __fastcall hkRenderBatchShadowMap(
        void* instance, void*, uint32_t batchMode, void* skinBatch, void* drawList,
        uint32_t drawIndex, void* skinSection, void* previousSection)
    {
        uint32_t boneCount = 0;
        uint32_t requestedInstances = 1;
        __try
        {
            bool sectionInRange = false;
            if (skinSection)
            {
                const auto* inst = static_cast<const m2::M2Instance*>(instance);
                const auto* model = inst ? reinterpret_cast<const uint8_t*>(inst->model) : nullptr;
                if (model)
                {
                    const auto* skin = *reinterpret_cast<uint8_t* const*>(model + m2::kOffModelSkin);
                    const auto* sections = *reinterpret_cast<uint8_t* const*>(model + m2::kOffModelSubmeshBuf);
                    const uint32_t sectionCount = skin ? *reinterpret_cast<const uint32_t*>(skin + 0x1C) : 0;
                    const auto* section = static_cast<const uint8_t*>(skinSection);
                    sectionInRange = sections && sectionCount && section >= sections &&
                        section < sections + static_cast<size_t>(sectionCount) *
                            sizeof(wxl::structure::m2::M2SkinSection);
                }
            }

            boneCount = sectionInRange
                ? static_cast<const wxl::structure::m2::M2SkinSection*>(skinSection)->boneCount
                : guard::bones::kMaxBonesPerDraw + 1u;

            if (drawList)
            {
                const auto* runs = *reinterpret_cast<uint32_t* const*>(drawList);
                if (runs)
                    requestedInstances =
                        runs[drawIndex * m2::kShadowRunStride + m2::kShadowRunCountField];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            boneCount = guard::bones::kMaxBonesPerDraw + 1u;
            requestedInstances = 1;
        }

        if (guard::BoneConstantsWouldOverflow(boneCount, requestedInstances))
        {
            const uint32_t skipped = ++g_shadowBoneOverflowSkips;
            if (skipped <= 32 || skipped % 1000u == 0)
            {
                char path[264] = "<unreadable>";
                __try
                {
                    const auto* inst = static_cast<const m2::M2Instance*>(instance);
                    const auto* model = inst ? reinterpret_cast<const m2::M2Model*>(inst->model) : nullptr;
                    if (model)
                    {
                        std::strncpy(path, model->pathStem, sizeof(path) - 1);
                        path[sizeof(path) - 1] = '\0';
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}

                WLOG_WARN("M2 shadow: skipped oversized palette bones=%u instances=%u max=%u draw=%u "
                          "model='%s' (skips=%u)",
                          boneCount, requestedInstances, guard::bones::kMaxBonesPerDraw,
                          drawIndex, path, skipped);
            }
            return;
        }

        if constexpr (wxl_modern_m2::kEnabled)
            wxl::runtime::m2shadow::OnShadowBatch(instance, skinSection);

        g_origRenderBatchShadowMap(instance, nullptr, batchMode, skinBatch, drawList,
                                   drawIndex, skinSection, previousSection);
    }

    /** @brief Rejects an unsafe main-draw doodad batch using the same co-instance budget. */
    void __fastcall hkDrawBatchDoodad(void* ctx, void* edx, void* elements, void* indices)
    {
        uint32_t boneCount = 0;
        uint32_t requestedInstances = 1;
        __try
        {
            const auto* c = static_cast<const gxoff::DrawBatchContext*>(ctx);
            boneCount = guard::CurrentBatchBoneCount(c);
            requestedInstances = guard::CurrentBatchRequestedInstances(c);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            boneCount = guard::bones::kMaxBonesPerDraw + 1u;
            requestedInstances = 1;
        }

        if (guard::BoneConstantsWouldOverflow(boneCount, requestedInstances))
        {
            const uint32_t skipped = ++g_doodadBoneOverflowSkips;
            if (skipped <= 32 || skipped % 1000u == 0)
                WLOG_WARN("M2 doodad-batch: skipped oversized palette bones=%u instances=%u max=%u "
                          "(skips=%u)", boneCount, requestedInstances,
                          guard::bones::kMaxBonesPerDraw, skipped);
            return;
        }

        g_origDrawBatchDoodad(ctx, edx, elements, indices);
    }
}

namespace wxl_modern_m2
{
    bool InstallM2CompatBones()
    {
        HookAttachByName("M2.BuildBonePalette", &hkBuildBonePalette, &g_origBuildBonePalette);
        const bool shadowHooked = HookAttachByName("M2.RenderBatchShadowMap",
                                                    &hkRenderBatchShadowMap, &g_origRenderBatchShadowMap);
        HookAttachByName("M2.DrawBatchDoodad", &hkDrawBatchDoodad, &g_origDrawBatchDoodad);
        if constexpr (kEnabled)
            wxl::runtime::m2shadow::Arm(shadowHooked);
        return true;
    }
}
