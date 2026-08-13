// Shared guard for M2 draw paths that upload bone matrices into the fixed VS constant range.
// Copyright (C) 2026 WarcraftXL

#pragma once

#include "../compat/BoneBudget.hpp"

#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "offsets/engine/Gx.hpp"

#include <cstdint>

namespace wxl_modern_m2
{
    namespace bones = wxl::modern::assets::common::bones;

    /** @brief True when a co-instanced palette cannot fit in the c31..c255 constant range. */
    constexpr bool BoneConstantsWouldOverflow(uint32_t boneCount, uint32_t instances) noexcept
    {
        // Division avoids overflowing the guard itself when a malformed count reaches this hook.
        return boneCount != 0 && instances > bones::kMaxBonesPerDraw / boneCount;
    }

    /** @brief Reads boneCount from the current batch record, not ctx->section from the previous draw. */
    inline uint32_t CurrentBatchBoneCount(const wxl::offsets::engine::gx::DrawBatchContext* ctx)
    {
        namespace gxoff = wxl::offsets::engine::gx;
        if (!ctx || !ctx->element) return 0;

        const auto* element = static_cast<const uint8_t*>(ctx->element);
        const void* section = *reinterpret_cast<void* const*>(element + gxoff::kM2ElementSectionField);
        if (!section) return 0;

        return static_cast<const wxl::structure::m2::M2SkinSection*>(section)->boneCount;
    }

    /** @brief Reads the co-instance count requested by the current batch record. */
    inline uint32_t CurrentBatchRequestedInstances(const wxl::offsets::engine::gx::DrawBatchContext* ctx)
    {
        namespace gxoff = wxl::offsets::engine::gx;
        if (!ctx || !ctx->element) return 1;
        return *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(ctx->element) + gxoff::kM2ElementRunLengthField);
    }

    static_assert(!BoneConstantsWouldOverflow(45, 1), "one 45-bone instance fits");
    static_assert(BoneConstantsWouldOverflow(45, 2), "two 45-bone instances overflow");
    static_assert(BoneConstantsWouldOverflow(76, 1), "one oversized palette overflows");
    static_assert(BoneConstantsWouldOverflow(0xFFFFFFFFu, 0xFFFFFFFFu), "malformed counts fail safe");
}
