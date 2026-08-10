// M2 bone compatibility: post-fill bone-palette event, and the shadow-batch and main-draw doodad-batch
// detours the client carries exactly one owner of each for.
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
#include "ShadowSpace.hpp"
#include "../compat/BoneBudget.hpp"

#include "engine/events/Event.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"

#include "offsets/engine/Gx.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>

namespace
{
    namespace ev    = wxl::events;
    namespace m2    = wxl::offsets::game::m2;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace bones = wxl::modern::assets::common::bones;

    m2::M2_BuildBonePaletteFn     g_origBuildBonePalette     = nullptr;
    m2::M2_RenderBatchShadowMapFn g_origRenderBatchShadowMap = nullptr;

    using DrawBatchDoodadFn = void (__fastcall*)(void* ctx, void* edx, void* elements, void* indices);
    DrawBatchDoodadFn g_origDrawBatchDoodad = nullptr;

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
     * @brief Detours the M2 ground-shadow batch draw.
     *
     * This detour is the ONLY one the client's real M2 ground-shadow draw can carry (MinHook
     * rejects a second on the same target), so the shadow bone probe rides it from here rather
     * than installing its own. Observe-only: it never alters the draw.
     */
    void __fastcall hkRenderBatchShadowMap(
        void* instance, void*, uint32_t batchMode, void* skinBatch, void* drawList,
        uint32_t drawIndex, void* skinSection, void* previousSection)
    {
        if constexpr (wxl_modern_m2::kEnabled)
            wxl::runtime::m2shadow::OnShadowBatch(instance, skinSection);

        g_origRenderBatchShadowMap(instance, nullptr, batchMode, skinBatch, drawList,
                                   drawIndex, skinSection, previousSection);
    }

    /**
     * @brief Detours the main-draw batched-doodad path, splitting an over-budget co-instance batch into
     *        several native calls instead of drawing it as one.
     *
     * The native function already loops internally over groups of AllocInstances' granted capacity, but
     * that capacity is sized for GPU buffer space, not for the c31-based VS-constant budget -- a group
     * can still ask for more than kMaxBonesPerDraw total bones across its co-instances. The fix mirrors
     * that same internal loop shape from the outside: shrink the batch record's run-length field
     * (kM2ElementRunLengthField) to a bone-budget-safe count per call, advance the indices pointer by
     * what was actually drawn, and restore the field to its original value before returning -- the
     * caller (CM2SceneRender::Draw) reads that same field a second time, right after this call returns,
     * to advance its own sorted-index cursor past the whole run.
     */
    void __fastcall hkDrawBatchDoodad(void* ctx, void* edx, void* elements, void* indices)
    {
        uint32_t* countField    = nullptr;
        uint32_t  originalCount = 0;
        uint32_t  chunkSize     = 0;
        __try
        {
            auto* c = static_cast<gxoff::DrawBatchContext*>(ctx);
            if (c->element)
            {
                auto* elementBytes = static_cast<uint8_t*>(c->element);
                countField    = reinterpret_cast<uint32_t*>(elementBytes + gxoff::kM2ElementRunLengthField);
                originalCount = *countField;

                const void* section =
                    *reinterpret_cast<void* const*>(elementBytes + gxoff::kM2ElementSectionField);
                const uint32_t boneCount = section
                    ? static_cast<const wxl::structure::m2::M2SkinSection*>(section)->boneCount
                    : 0;

                chunkSize = boneCount > 0
                    ? std::max<uint32_t>(1u, bones::kMaxBonesPerDraw / boneCount)
                    : originalCount;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            countField = nullptr; // fail safe: single native call below, batch record left untouched
        }

        if (!countField || chunkSize >= originalCount)
        {
            g_origDrawBatchDoodad(ctx, edx, elements, indices);
            return;
        }

        auto*    indexBytes = static_cast<uint8_t*>(indices);
        uint32_t drawn       = 0;
        while (drawn < originalCount)
        {
            const uint32_t thisChunk = std::min(chunkSize, originalCount - drawn);
            *countField = thisChunk;
            g_origDrawBatchDoodad(ctx, edx, elements, indexBytes + static_cast<size_t>(drawn) * 4);
            drawn += thisChunk;
        }
        *countField = originalCount;
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
