// modern-m2 host face: register the byte-transform so a source M2 is reshaped on the host before serve.
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

#include "Host.hpp"

#include "../shared/Downport.hpp"
#include "../shared/Md21.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

/**
 * @brief The module's host face: plugs the shared byte-transform into the host's Transform hook.
 *
 * The DLL and the host run the same downport code, so a model the host reshapes is byte-for-byte what the
 * in-process DLL fallback would produce. The live-engine half (skin rebuild, draw fixups) has no host form and
 * stays in the DLL.
 */
namespace
{
    namespace dp  = wxl::scripts::modernm2::downport;
    namespace m21 = wxl::scripts::modernm2::md21;

    /**
     * @brief Host Transform hook: reshapes a source M2 onto the client contract.
     * @param name Asset name (unused).
     * @param raw  Source asset bytes.
     * @param out  Receives the reshaped image on success.
     * @return true if the asset was reshaped; false (pass) for anything that is not a source image, so the
     *         host serves it raw.
     */
    bool Transform(std::string_view name, std::span<const uint8_t> raw, std::vector<uint8_t>& out)
    {
        (void)name;
        const uint32_t size = static_cast<uint32_t>(raw.size());

        if (m21::IsMd21(raw))
        {
            std::vector<uint8_t> md20;
            if (!m21::Dechunk(raw, &wxl::host::ResolveFdid, md20)) return false;
            const uint32_t orig = static_cast<uint32_t>(md20.size());
            const uint32_t work = dp::WorkSize(md20.data(), orig);
            md20.resize(work);
            if (!dp::ProcessInPlace(md20.data(), orig, work)) return false;
            m21::ZeroBoneLookup(md20.data(), static_cast<uint32_t>(md20.size()));
            out = std::move(md20);
            return true;
        }

        if (!dp::IsConvertible(raw.data(), size)) return false;

        const uint32_t workSize = dp::WorkSize(raw.data(), size);
        out.resize(workSize);
        std::memcpy(out.data(), raw.data(), size);
        if (!dp::ProcessInPlace(out.data(), size, workSize)) { out.clear(); return false; }
        return true;
    }

    /**
     * @brief File-scope registrar that self-registers the Transform hook at host startup, before main mounts
     *        the archives.
     */
    struct Registrar
    {
        Registrar() { wxl::host::RegisterTransform("modern-m2", &Transform); }
    };
    Registrar g_registrar;
}
