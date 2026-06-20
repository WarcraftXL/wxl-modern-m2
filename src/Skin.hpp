// Skin: rebuild the material / texunit contract a source skin omits, at skin finalize.
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

#include "game/m2/M2.hpp"
#include "structure/m2/M2Format.hpp"

/**
 * @brief Rebuilds the material / texunit contract a source skin omits, at skin finalize.
 *
 * A source skin encodes each batch's material in its own shaderId (packed blend-bit pair, or a modern effect
 * index) and leaves the header textureUnitLookup empty. The client's first shader-id pass indexes
 * textureUnitLookup[batch.textureCoordComboIndex] and the second texture binds only when the contract is
 * present. This runs at skin finalize, where the header and the pointer-fixed skin are live and before the
 * native passes size their parallel batch blocks: it splits over-budget submeshes (bones theme), decodes each
 * batch's shaderId into the fixed-function blend bits the client understands, synthesizes textureUnitLookup
 * and textureCombinerCombos, parks LOD submeshes, and clamps the render flags.
 */
namespace wxl::scripts::modernm2::skin
{
    /**
     * @brief Rebuilds the material / texunit contract on a downported model's live skin profile.
     * @param md   Parsed model header (arrays are raw pointers by this point).
     * @param skin Attached live skin profile.
     * @param name Model path, used for logging.
     */
    void Rebuild(wxl::structure::m2::M2Header* md, wxl::game::m2::M2SkinProfile* skin, const char* name);
}
