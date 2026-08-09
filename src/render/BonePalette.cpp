// M2 bone compatibility: post-fill bone-palette event and an SM3 constant-cache guard for both draw
// paths (shadow batches and main-draw doodad batches) that can overrun it.
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

    m2::M2_BuildBonePaletteFn     g_origBuildBonePalette     = nullptr;
    m2::M2_RenderBatchShadowMapFn g_origRenderBatchShadowMap = nullptr;
    std::atomic<uint32_t>         g_shadowBoneOverflowSkips{ 0 };

    using DrawBatchDoodadFn = void (__fastcall*)(void* ctx, void* edx, void* elements, void* indices);
    DrawBatchDoodadFn      g_origDrawBatchDoodad      = nullptr;
    std::atomic<uint32_t>  g_doodadBoneOverflowSkips{ 0 };

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
     * @brief Rejects M2 shadow batches whose palette would overrun WoW's VS constant cache, or whose
     *        skinSection pointer has fallen outside the model's own submesh-copy array.
     *
     * The native shadow path begins at c31 and copies three float4 registers per bone. The cache contains
     * c0..c255, so 75 bones is the largest representable palette FOR A SINGLE INSTANCE -- but the native
     * copy loop runs once per co-instance the engine batched into this one shadow draw call, writing every
     * instance's bones back to back into the same never-reset destination, so the real overflow condition
     * is boneCount * (instances batched into this draw) > 75, not boneCount alone. A model comfortably
     * under 75 bones on its own still overflows once enough identical placements land in the same shadow
     * batch (a load-burst wave that coerces a cluster of placements onto the same coarse model being the
     * case that surfaced this). Letting the native function process an oversized batch overwrites the
     * adjacent Gx vertex-declaration table with bone-matrix floats and crashes later in GxPrimVertexPtr.
     *
     * skinSection itself is not handed in directly: the native batch-list walk
     * (CM2Model::RenderModelBatchListShadowMap) derives it as
     * model+kOffModelSubmeshBuf + batch.skinSectionIndex * sizeof(M2SkinSection) -- an index into an
     * array kFinalizeSkin sizes to the CURRENTLY attached skin's submeshCount. A batch whose
     * skinSectionIndex the live array no longer covers lands on other still-mapped heap memory, which
     * SEH does not catch (it only catches genuine access violations, not a pointer that is simply
     * wrong): the read below would appear to succeed with garbage. Range-checking the pointer against
     * the model's own submeshCount catches that case before any field of it is trusted.
     */
    void __fastcall hkRenderBatchShadowMap(
        void* instance, void*, uint32_t batchMode, void* skinBatch, void* drawList,
        uint32_t drawIndex, void* skinSection, void* previousSection)
    {
        using wxl_modern_m2::kMaxShadowBones;
        constexpr size_t kSkinSectionStride = 0x30;
        uint32_t boneCount = 0;
        __try
        {
            bool inRange = false;
            if (skinSection)
            {
                const auto* inst  = static_cast<const m2::M2Instance*>(instance);
                const auto* model = inst ? reinterpret_cast<const uint8_t*>(inst->model) : nullptr;
                if (model)
                {
                    const auto* skin = *reinterpret_cast<uint8_t* const*>(model + m2::kOffModelSkin);
                    const auto* runtime = *reinterpret_cast<uint8_t* const*>(model + m2::kOffModelSubmeshBuf);
                    const uint32_t submeshCount = skin ? *reinterpret_cast<const uint32_t*>(skin + 0x1C) : 0;
                    const auto* sec = static_cast<const uint8_t*>(skinSection);
                    inRange = runtime && submeshCount &&
                              sec >= runtime && sec < runtime + static_cast<size_t>(submeshCount) * kSkinSectionStride;
                }
            }

            // boneCount (the submesh's palette size, what the shadow path copies 3 registers per
            // bone of) -- NOT boneInfluences (max bone weights per VERTEX, always small, ~1-4, and
            // useless as an overflow signal). A previous version of this guard checked the wrong
            // field, so it never actually rejected an oversized palette.
            boneCount = inRange ? static_cast<const wxl::structure::m2::M2SkinSection*>(skinSection)->boneCount
                                 : kMaxShadowBones + 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            boneCount = kMaxShadowBones + 1;
        }

        // The co-instance count this draw was asked to batch -- the native function itself reads the
        // identical field, from the identical drawList/drawIndex, before deciding how many co-instances
        // to actually grant (which can only clamp it lower, never raise it -- so this is always a safe,
        // conservative upper bound on the real batch size, never an undercount). Any read failure fails
        // safe by forcing the skip below rather than risking an unguarded batch.
        uint32_t requestedInstances = 1;
        __try
        {
            if (drawList)
            {
                const auto* const* listBase = reinterpret_cast<void* const* const*>(drawList);
                const auto* runs = reinterpret_cast<const uint32_t*>(*listBase);
                if (runs)
                    requestedInstances = runs[drawIndex * 3 + 2];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            requestedInstances = kMaxShadowBones + 1;
        }

        if (wxl_modern_m2::BoneConstantsWouldOverflow(boneCount, requestedInstances))
        {
            const uint32_t skipped = ++g_shadowBoneOverflowSkips;
            if (skipped <= 32 || (skipped % 1000u) == 0)
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
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    std::strcpy(path, "<unreadable>");
                }
                WLOG_WARN("M2 shadow: skipped oversized palette bones=%u instances=%u max=%u draw=%u "
                          "model='%s' (skips=%u)",
                          boneCount, requestedInstances, kMaxShadowBones, drawIndex, path, skipped);
            }
            return;
        }

        // This detour is the ONLY one the client's real M2 ground-shadow draw can carry (MinHook
        // rejects a second on the same target), so the shadow bone probe rides it from here rather
        // than installing its own. Observe-only: it never alters the draw.
        if constexpr (wxl_modern_m2::kEnabled)
            wxl::runtime::m2shadow::OnShadowBatch(instance, skinSection);

        g_origRenderBatchShadowMap(instance, nullptr, batchMode, skinBatch, drawList,
                                   drawIndex, skinSection, previousSection);
    }

    /**
     * @brief Same overflow guard as hkRenderBatchShadowMap above, for the main-draw doodad-batch path.
     *
     * The shadow path is not the only caller that can overrun the VS constant cache this way -- the
     * main-draw batched-doodad path negotiates a co-instance batch and copies bone palettes into the
     * exact same c31-based constant range the same unbounded, never-reset way, and nothing guarded it
     * until now. Since the overrun corrupts a single process-wide table (not anything per-draw-call),
     * an overflow here can just as easily be what a LATER, otherwise-ordinary shadow or main-draw batch
     * crashes reading back -- the two guards close both known ways into the same shared corruption.
     */
    void __fastcall hkDrawBatchDoodad(void* ctx, void* edx, void* elements, void* indices)
    {
        using namespace wxl_modern_m2;
        uint32_t boneCount = 0, requestedInstances = 1;
        __try
        {
            const auto* c = static_cast<const gxoff::DrawBatchContext*>(ctx);
            boneCount = CurrentBatchBoneCount(c);
            requestedInstances = CurrentBatchRequestedInstances(c);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            boneCount = kMaxShadowBones + 1;
        }

        if (BoneConstantsWouldOverflow(boneCount, requestedInstances))
        {
            const uint32_t skipped = ++g_doodadBoneOverflowSkips;
            if (skipped <= 32 || (skipped % 1000u) == 0)
                WLOG_WARN("M2 doodad-batch: skipped oversized palette bones=%u instances=%u max=%u (skips=%u)",
                          boneCount, requestedInstances, kMaxShadowBones, skipped);
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
