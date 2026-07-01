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

#include "core/Logger.hpp"
#include "structure/m2/M2Format.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
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
    namespace fmt = wxl::structure::m2;

    constexpr uint32_t kBoneStride = 0x58;

    bool StartsWithCI(std::string_view value, std::string_view prefix)
    {
        if (prefix.size() > value.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
        {
            const auto a = static_cast<unsigned char>(value[i] == '/' ? '\\' : value[i]);
            const auto b = static_cast<unsigned char>(prefix[i]);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
        return true;
    }

    bool IsSkinnedActorPath(std::string_view name)
    {
        return StartsWithCI(name, "creature\\") || StartsWithCI(name, "character\\") ||
               StartsWithCI(name, "item\\");
    }

    uint32_t Rd32(const uint8_t* p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    void Wr32(uint8_t* p, uint32_t v)
    {
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        p[2] = uint8_t(v >> 16);
        p[3] = uint8_t(v >> 24);
    }

    void WrFloat(uint8_t* p, float v)
    {
        static_assert(sizeof(float) == 4);
        std::memcpy(p, &v, sizeof(v));
    }

    std::string LowerSlashed(std::string_view name)
    {
        std::string s(name);
        for (char& c : s)
            c = (c == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool HelmRaceId(std::string_view name, std::string& id)
    {
        const std::string s = LowerSlashed(name);
        const size_t slash = s.find_last_of('\\');
        const std::string_view base(s.data() + (slash == std::string::npos ? 0 : slash + 1),
                                    s.size() - (slash == std::string::npos ? 0 : slash + 1));

        size_t ext = base.rfind('.');
        if (ext == std::string_view::npos) ext = base.size();
        if (ext < 3) return false;
        if (base.substr(ext) != ".m2" && base.substr(ext) != ".mdx") return false;
        if (base.rfind("helm_", 0) != 0 && base.rfind("helmet_", 0) != 0
            && base.find("_helm_") == std::string_view::npos) return false;

        if (ext >= 4 && base[ext - 2] == '_')
        {
            id.assign(base.substr(ext - 4, 2));
            id.push_back(base[ext - 1]);
        }
        else
        {
            id.assign(base.substr(ext - 3, 3));
        }
        const std::string_view race = std::string_view(id).substr(0, 2);
        const char sex = id[2];
        if (sex != 'm' && sex != 'f') return false;

        static constexpr std::string_view races[] = {
            "be", "dr", "dw", "gn", "hu", "ni", "or", "sc", "ta", "tr", "sk", "go"
        };
        for (std::string_view r : races)
            if (race == r) return true;
        return false;
    }

    void HelmOffsetForId(const std::string& id, float& x, float& z)
    {
        x = -0.0587258f;
        z = -0.18623257f;
        if (id == "drf") { x = -0.0587258f; z = -0.195f; }
        else if (id == "drm") { x = -0.0587258f; z = -0.245f; }
        else if (id == "taf") { x = -0.13f; z = -0.1f; }
        else if (id == "tam") { x = -0.2f; z = -0.1f; }
        else if (id == "nim") { x = -0.09f; z = -0.18f; }
        else if (id == "nif") { x = -0.08f; z = -0.195f; }
        else if (id == "orf") { x = -0.08f; z = -0.171f; }
        else if (id == "orm") { x = -0.13f; z = -0.21f; }
        else if (id == "trf") { x = -0.0887258f; z = -0.08623257f; }
        else if (id == "trm") { x = -0.13f; z = -0.16f; }
        else if (id == "bef") { x = 0.01f; z = -0.2f; }
        else if (id == "bem") { x = -0.08f; z = -0.165f; }
        else if (id == "huf") { x = -0.09f; z = -0.18f; }
        else if (id == "scm") { x = -0.12f; z = -0.12623256f; }
        else if (id == "scf") { x = -0.01f; z = -0.15f; }
        else if (id == "gnf") { x = -0.015f; z = -0.263f; }
        else if (id == "gnm") { x = -0.009f; z = -0.23f; }
        else if (id == "dwm") { x = -0.0227258f; z = -0.1725f; }
        else if (id == "dwf") { x = 0.01f; z = -0.195f; }
    }

    bool ApplyHelmOffset(std::string_view name, std::vector<uint8_t>& model)
    {
        std::string id;
        if (!HelmRaceId(name, id)) return false;
        if (model.size() < sizeof(fmt::M2Header)) return false;

        auto* md = reinterpret_cast<fmt::M2Header*>(model.data());
        if (md->magic != fmt::kMagicMD20 || md->bones.count == 0 || md->bones.offset == 0) return false;
        const uint32_t boneCount = md->bones.count;
        const uint32_t boneOffset = md->bones.offset;

        const uint64_t bonesEnd = uint64_t(boneOffset) + uint64_t(boneCount) * kBoneStride;
        if (bonesEnd > model.size() || bonesEnd < boneOffset) return false;
        if (uint64_t(model.size()) + uint64_t(boneCount) * 32u > 0xffffffffu) return false;

        float x, z;
        HelmOffsetForId(id, x, z);

        for (uint32_t i = 0; i < boneCount; ++i)
        {
            uint8_t* bone = model.data() + boneOffset + i * kBoneStride;
            Wr32(bone + 0x04, Rd32(bone + 0x04) | 0x200u);

            const uint32_t timestampArray = static_cast<uint32_t>(model.size());
            model.resize(model.size() + 32, 0);
            uint8_t* data = model.data() + timestampArray;

            bone = model.data() + boneOffset + i * kBoneStride;
            Wr32(bone + 0x14, 1);
            Wr32(bone + 0x18, timestampArray);
            Wr32(data + 0x00, 1);
            Wr32(data + 0x04, timestampArray + 0x08);

            const uint32_t valueArray = timestampArray + 0x0c;
            Wr32(bone + 0x1c, 1);
            Wr32(bone + 0x20, valueArray);
            Wr32(data + 0x0c, 1);
            Wr32(data + 0x10, valueArray + 0x08);
            WrFloat(data + 0x14, x);
            WrFloat(data + 0x18, 0.0f);
            WrFloat(data + 0x1c, z);
        }

        wxl::core::log::Printf("modern-m2: %.*s applied helm offset id=%s bones=%u",
                               int(name.size()), name.data(), id.c_str(), boneCount);
        return true;
    }

    /**
     * @brief Host Transform hook: reshapes a source M2 onto the client contract.
     * @param name Asset name.
     * @param raw  Source asset bytes.
     * @param out  Receives the reshaped image on success.
     * @return true if the asset was reshaped; false (pass) for anything that is not a source image, so the
     *         host serves it raw.
     */
    bool Transform(std::string_view name, std::span<const uint8_t> raw, std::vector<uint8_t>& out)
    {
        const uint32_t size = static_cast<uint32_t>(raw.size());

        if (m21::IsMd21(raw))
        {
            std::vector<uint8_t> md20;
            if (!m21::Dechunk(raw, &wxl::host::ResolveFdid, md20)) return false;
            const uint32_t orig = static_cast<uint32_t>(md20.size());
            const uint32_t work = dp::WorkSize(md20.data(), orig);
            md20.resize(work);
            if (!dp::ProcessInPlace(md20.data(), orig, work)) return false;
            if (!IsSkinnedActorPath(name))
                m21::ZeroBoneLookup(md20.data(), static_cast<uint32_t>(md20.size()));
            ApplyHelmOffset(name, md20);
            out = std::move(md20);
            return true;
        }

        if (!dp::IsConvertible(raw.data(), size)) return false;

        const uint32_t workSize = dp::WorkSize(raw.data(), size);
        out.resize(workSize);
        std::memcpy(out.data(), raw.data(), size);
        if (!dp::ProcessInPlace(out.data(), size, workSize)) { out.clear(); return false; }
        ApplyHelmOffset(name, out);
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
