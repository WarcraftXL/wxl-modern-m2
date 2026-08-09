// Shared VS-constant-cache overflow check for every M2 draw path that copies a bone palette into
// shader constants starting at c31 -- CM2Model::RenderBatchShadowMap, CM2SceneRender::DrawBatch, and
// CM2SceneRender::DrawBatchDoodad all do this, none of them bound it natively, and the same mistake
// (reading a stale copy of the section pointer instead of the current batch's own) cost a full
// debugging pass to find once already -- see BonePalette.cpp's own top comment for the incident. One
// canonical threshold and one canonical "read the CURRENT batch's boneCount" helper instead of three
// hand-copied instances is the point of this file.
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

#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "offsets/engine/Gx.hpp"

#include <cstdint>

namespace wxl_modern_m2
{
    // The hardware VS constant cache is c0..c255; every one of the three copy loops starts at c31 and
    // spends 3 float4 registers per bone, so this is the largest palette (times however many
    // co-instances a draw batches together) any of them can represent.
    inline constexpr uint32_t kMaxShadowBones = (256u - 31u) / 3u;

    /** @brief True when copying boneCount bones for each of `instances` co-instances would overrun the cache. */
    inline bool BoneConstantsWouldOverflow(uint32_t boneCount, uint32_t instances)
    {
        return boneCount * instances > kMaxShadowBones;
    }

    /**
     * @brief Reads boneCount off a draw context's CURRENT batch record (ctx->element->section), NEVER
     *        the draw context's own cached section field (kDrawBatchCtxSectionField / ctx+0x90) --
     *        that field is only overwritten by the native draw function's own entry code, so a
     *        pre-check hook reading it before calling original still sees whichever batch drew last
     *        through the same reused context, not the one about to run. See
     *        kDrawBatchCtxSectionField's own doc comment (offsets/engine/Gx.hpp) for the full
     *        mechanism this works around.
     * @return 0 if ctx->element or its section is null -- the caller decides the fail-safe default.
     */
    inline uint32_t CurrentBatchBoneCount(const wxl::offsets::engine::gx::DrawBatchContext* ctx)
    {
        namespace gxoff = wxl::offsets::engine::gx;
        if (!ctx->element) return 0;
        const auto* elementBytes = static_cast<const uint8_t*>(ctx->element);
        const void* section = *reinterpret_cast<void* const*>(elementBytes + gxoff::kM2ElementSectionField);
        if (!section) return 0;
        return static_cast<const wxl::structure::m2::M2SkinSection*>(section)->boneCount;
    }

    /// The co-instance count CM2Shared::AllocInstances is about to be asked to grant for the current
    /// batch. Unlike the section pointer above, this was never read from a stale field -- it already
    /// comes straight off the same batch record (ctx->element+0x1C) -- so this is purely a namesake
    /// for readability at the call site, not a bug fix.
    inline uint32_t CurrentBatchRequestedInstances(const wxl::offsets::engine::gx::DrawBatchContext* ctx)
    {
        if (!ctx->element) return 1;
        return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(ctx->element) + 0x1C);
    }
}
