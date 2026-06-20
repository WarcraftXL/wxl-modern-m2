// Synthesizes the texture-coordinate-combos array a source model omits.
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

#include "structure/m2/M2Format.hpp"

/**
 * @brief Synthesizes the texture-coordinate-combos array a source model omits.
 *
 * The client's skin finalize indexes textureCoordCombos (header.textureUnitLookup) unconditionally;
 * source creatures often ship it empty. This theme synthesizes a one-entry array so the parse-time
 * finalize does not index a null array (the in-memory rebuild later replaces it with the full contract).
 */
namespace wxl::scripts::modernm2::textures
{
    /**
     * @brief Reports whether the model omits the texture-coordinate-combos array.
     * @param md  Model header.
     * @return True if a one-entry synth is needed.
     */
    bool NeedsCoordCombos(const wxl::structure::m2::M2Header* md);

    /**
     * @brief Writes a one-entry texture-coordinate-combos array (set 0 = standard, no env) and points
     *        header.textureUnitLookup at it.
     * @param md            Model header. The caller must have reserved 2 bytes at appendOffset.
     * @param appendOffset  Model-relative offset where the entry is written.
     */
    void SynthCoordCombos(wxl::structure::m2::M2Header* md, uint32_t appendOffset);
}
