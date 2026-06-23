// MD21 container: de-chunk a chunked modern M2 to a self-contained MD20 the Client loader reads.
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

#include "Md21.hpp"

#include "Contract.hpp"
#include "core/Logger.hpp"
#include "structure/m2/M2Format.hpp"

#include <string>

namespace wxl::scripts::modernm2::md21
{
    namespace fmt = wxl::structure::m2;

    namespace
    {
        // Auxiliary chunk magics are stored in memory order (not reversed), so a plain little-endian read
        // matches these.
        constexpr uint32_t Magic(char a, char b, char c, char d)
        {
            return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
                   (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
        }
        constexpr uint32_t kTXID = Magic('T', 'X', 'I', 'D');

        // M2 header field offsets the de-chunk touches.
        constexpr uint32_t kHdrTextures   = 0x50; // M2Array: texture records
        constexpr uint32_t kHdrBoneCombos = 0x78; // M2Array: bone lookup table
        // M2Texture record (0x10): type@0x00, flags@0x04, filename {count@0x08, offset@0x0C}.
        constexpr uint32_t kTexStride     = 0x10;
        constexpr uint32_t kTexNameCount  = 0x08;
        constexpr uint32_t kTexNameOffset = 0x0C;
    }

    bool IsMd21(std::span<const uint8_t> in)
    {
        return in.size() >= 8 && Rd32(in.data()) == fmt::kMagicMD21;
    }

    bool Dechunk(std::span<const uint8_t> in, TxidResolver resolve, std::vector<uint8_t>& out)
    {
        const uint8_t* b = in.data();
        const uint32_t n = static_cast<uint32_t>(in.size());
        if (n < 8 || Rd32(b) != fmt::kMagicMD21)
            return false;

        const uint32_t md20Size = Rd32(b + 4);
        if (8 + size_t(md20Size) > n || md20Size < sizeof(fmt::M2Header))
            return false;

        // Collect the TXID texture FileDataIDs from the chunks trailing the MD20 block.
        std::vector<uint32_t> txid;
        for (uint32_t o = 8 + md20Size; o + 8 <= n; )
        {
            const uint32_t tag = Rd32(b + o), sz = Rd32(b + o + 4);
            if (o + 8 + size_t(sz) > n) break;
            if (tag == kTXID)
                for (uint32_t k = 0; k + 4 <= sz; k += 4) txid.push_back(Rd32(b + o + 8 + k));
            o += 8 + sz;
        }

        // The Client reads MD20-relative offsets, so the extracted block can grow at its tail (the inlined
        // names) without disturbing any existing offset.
        out.assign(b + 8, b + 8 + md20Size);

        const uint32_t texCount = Rd32(out.data() + kHdrTextures);
        const uint32_t texOfs   = Rd32(out.data() + kHdrTextures + 4);
        uint32_t hardcoded = 0, inlined = 0;
        for (uint32_t i = 0; i < texCount; ++i)
        {
            const uint32_t rec = texOfs + i * kTexStride;
            if (rec + kTexStride > out.size()) break;
            if (Rd32(out.data() + rec) != fmt::kTexTypeHardcoded) continue; // not a named texture
            if (Rd32(out.data() + rec + kTexNameCount) != 0) continue;      // already has an inline name
            ++hardcoded;

            std::string path;
            if (i >= txid.size() || !txid[i] || !resolve || !resolve(txid[i], path) || path.empty())
                continue;

            const uint32_t nameOfs = static_cast<uint32_t>(out.size());
            out.insert(out.end(), path.begin(), path.end());
            out.push_back(0);
            Wr32(out.data() + rec + kTexNameCount,  static_cast<uint32_t>(path.size()) + 1);
            Wr32(out.data() + rec + kTexNameOffset, nameOfs);
            ++inlined;
        }

        wxl::core::log::Printf("modern-m2 md21: md20=%u textures=%u hardcoded=%u inlined=%u txid=%zu",
            md20Size, texCount, hardcoded, inlined, txid.size());
        return true;
    }

    void ZeroBoneLookup(uint8_t* md20, uint32_t size)
    {
        const uint32_t count = Rd32(md20 + kHdrBoneCombos);
        const uint32_t ofs   = Rd32(md20 + kHdrBoneCombos + 4);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (ofs + i * 2 + 2 > size) break;
            Wr16(md20 + ofs + i * 2, 0);
        }
    }
}
