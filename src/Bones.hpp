// Bones: partition a submesh whose per-draw bone palette exceeds the client ceiling into sub-sections.
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

#include <cstdint>
#include <vector>

#include "game/m2/M2.hpp"
#include "structure/m2/M2Format.hpp"

/**
 * @brief Partitions any drawn submesh whose per-draw bone palette exceeds the client ceiling into
 *        sub-sections sized within the ceiling.
 *
 * The client vertex shader holds the bone palette from register 31, 3 registers per matrix, so
 * (256 - 31) / 3 = 75 bones per draw is the ceiling. A greedy triangle bin-packer splits each over-ceiling
 * submesh into sub-sections, each with its own compact boneCombos slice, deduplicated vertex block (bones[]
 * remapped to the slice), and global-indexed triangle block, rebuilding the live skin geometry arrays and
 * header.boneCombos. The skin theme re-points each batch across the run of sub-sections its original submesh
 * became.
 */
namespace wxl::scripts::modernm2::bones
{
    // The client per-draw bone budget. A submesh past it is split (or, when a split is skipped, clamped).
    constexpr uint16_t kMaxBonesPerDraw = 75;
    // A submesh / batch count past this is treated as malformed; the commit is capped well under any value
    // that would overflow the native sizing.
    constexpr uint32_t kMaxBatches = 0x4000;

    /** @brief One rebuilt sub-section plus the original submesh it came from. */
    struct SplitSection { wxl::structure::m2::M2SkinSection section; uint16_t origSubmesh; };
    /** @brief The contiguous run of new sub-section indices one original submesh became. */
    struct SplitRun { uint16_t first; uint16_t count; };

    /**
     * @brief Partitions over-ceiling submeshes, rebuilding the live skin geometry and header.boneCombos.
     * @param md          Parsed model header (boneCombos array is a raw pointer here).
     * @param skin        Live skin profile whose geometry arrays are rebuilt on success.
     * @param outSections Receives the rebuilt sub-sections.
     * @param splitMap    Receives the per-original-submesh sub-section run, indexed by original submesh.
     * @param splitCount  Receives the count of extra sub-draws produced.
     * @param name        Model path, used for logging.
     * @return true on commit; false (no commit) on any overflow, allocation failure, or missing array, leaving
     *         the caller on the clamp path.
     */
    bool SplitSubmeshes(wxl::structure::m2::M2Header* md, wxl::game::m2::M2SkinProfile* skin,
                        std::vector<SplitSection>& outSections, std::vector<SplitRun>& splitMap,
                        uint32_t& splitCount, const char* name);
}
