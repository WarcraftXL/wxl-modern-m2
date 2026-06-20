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

#include "Textures.hpp"

#include "Contract.hpp"

namespace wxl::scripts::modernm2::textures
{
    namespace fmt = wxl::structure::m2;

    /**
     * @brief Reports whether the model omits the texture-coordinate-combos array.
     * @param md  Model header.
     * @return True if a one-entry synth is needed.
     */
    bool NeedsCoordCombos(const fmt::M2Header* md)
    {
        return md->textureUnitLookup.count == 0;
    }

    /**
     * @brief Writes a one-entry texture-coordinate-combos array and points header.textureUnitLookup at it.
     * @param md            Model header. The caller must have reserved 2 bytes at appendOffset.
     * @param appendOffset  Model-relative offset where the entry is written.
     */
    void SynthCoordCombos(fmt::M2Header* md, uint32_t appendOffset)
    {
        Wr16(md->base() + appendOffset, 0);    // one entry = texture-coordinate set 0 (standard, no env)
        md->textureUnitLookup.count  = 1;      // header+0x88
        md->textureUnitLookup.offset = appendOffset; // header+0x8C -> the appended entry
    }
}
