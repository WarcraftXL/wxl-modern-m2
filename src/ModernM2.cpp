// wxl-modern-m2: module bring-up + binding the core events to the M2 themes.
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

#include "ModernM2.hpp"

#include "core/Logger.hpp"
#include "game/m2/M2.hpp"
#include "structure/m2/M2Format.hpp"

#include "../shared/Contract.hpp"
#include "../shared/Downport.hpp"
#include "../shared/Particles.hpp"
#include "../shared/Ribbons.hpp"
#include "Skin.hpp"

#include <cstring>

namespace wxl::scripts::modernm2
{
    namespace ev  = wxl::events;
    namespace m2  = wxl::game::m2;
    namespace fmt = wxl::structure::m2;

    /**
     * @brief Binds the module's handlers to the core M2 events.
     */
    ModernM2::ModernM2()
    {
        on<&ModernM2::OnModelLoadPre>(ev::Event::OnModelLoadPre);
        on<&ModernM2::OnModelLoad>(ev::Event::OnModelLoad);
        on<&ModernM2::OnSkinFinalize>(ev::Event::OnM2SkinFinalize);
        on<&ModernM2::OnSetupBatchAlpha>(ev::Event::OnM2SetupBatchAlpha);
        on<&ModernM2::OnRibbonDraw>(ev::Event::OnRibbonDraw);

        WLOG_INFO("wxl-modern-m2: loaded (in-memory modern M2 asset support)");
    }

    /**
     * @brief Reshapes the .m2 bytes in the model's load buffer onto the native version before the parser runs,
     *        and registers the model for the live-engine half.
     *
     * The byte-transform may have already run on the host (the image arrives compacted with its inner version
     * staged) or it runs here in process. Either way the model is finalized to the native version and
     * registered for the skin rebuild at finalize and the draw-time fixups.
     * @param a Model load arguments carrying the model pointer and load buffer.
     */
    void ModernM2::OnModelLoadPre(const ev::ModelLoadArgs& a)
    {
        // A fresh model now occupies this pointer; drop any registration left from a prior model freed at
        // the same address (only a successful path below re-registers it).
        Forget(a.model);

        void* buf = m2::FileBuffer(a.model);
        const uint32_t size = m2::FileSize(a.model);
        if (!buf || size < sizeof(fmt::M2Header)) return;
        auto* md = static_cast<fmt::M2Header*>(buf);
        if (md->magic != fmt::kMagicMD20) return;

        // Host already compacted it: the records are on the client contract and the version is staged. Clear
        // the staging bit to hand the parser the clean native version, then register it.
        if (IsStagedVersion(md->version))
        {
            md->version &= ~kStagedVersionBit;
            Remember(a.model);
            return;
        }

        // Host off (or a file the host did not serve): reshape in process. Downport never allocates; the
        // caller owns the buffer. Reshape within the load buffer when the image does not grow; otherwise
        // allocate a replacement with the model allocator (so the destructor frees it), copy, reshape, swap.
        if (!downport::IsConvertible(buf, size)) return;
        downport::Inspect(buf, size);

        const uint32_t workSize = downport::WorkSize(buf, size);
        void* image = buf;
        if (workSize == size)
        {
            if (!downport::ProcessInPlace(buf, size, size)) return;
        }
        else
        {
            void* out = m2::AllocBuffer(workSize, ".\\wxl-modern-m2");
            if (!out) return;
            std::memcpy(out, buf, size);
            if (!downport::ProcessInPlace(out, size, workSize)) { m2::FreeBuffer(out); return; }
            m2::ReplaceBuffer(a.model, out, workSize); // parser now reads the grown image
            m2::FreeBuffer(buf);                        // release the original source bytes
            image = out;
        }

        // ProcessInPlace staged the version; finalize it to the native value so the parser accepts it, then
        // register the model (no longer distinguishable by version at draw time) for the live-engine half.
        static_cast<fmt::M2Header*>(image)->version &= ~kStagedVersionBit;
        Remember(a.model);
        WLOG_INFO("modern-m2: reshaped M2 to 264 (%u -> %u bytes)", size, workSize);
    }

    /**
     * @brief Parsed-model load hook.
     * @param a Model load arguments.
     */
    void ModernM2::OnModelLoad(const ev::ModelLoadArgs& a)
    {
        (void)a; // parsed-model hook for fixups that need the parsed object
    }

    /**
     * @brief Rebuilds the material / texunit contract before native finalize sizes its blocks, scoped to
     *        models this module reshaped.
     *
     * The source skin's batches carry modern shaderIds and an empty header textureUnitLookup; the rebuild
     * synthesizes the contract the client shader-id pass indexes. Scoped to reshaped models, which assume the
     * source batch encoding.
     * @param a Skin finalize arguments carrying the model pointer.
     */
    void ModernM2::OnSkinFinalize(const ev::M2SkinFinalizeArgs& a)
    {
        if (!WasReshaped(a.model)) return;
        auto* md = m2::Header(a.model);
        auto* sk = m2::Skin(a.model);
        if (md && sk)
            skin::Rebuild(md, sk, "");
    }

    /**
     * @brief Delegates the draw-time alpha-key fixup to the particles theme, flagged for reshaped models.
     * @param a Batch-alpha setup arguments.
     */
    void ModernM2::OnSetupBatchAlpha(const ev::M2SetupBatchAlphaArgs& a)
    {
        particles::OnSetupBatchAlpha(a, WasReshaped(a.model));
    }

    /**
     * @brief Delegates the ribbon draw fixup to the ribbons theme.
     * @param a Ribbon draw arguments.
     */
    void ModernM2::OnRibbonDraw(const ev::RibbonDrawArgs& a)
    {
        ribbons::OnRibbonDraw(a);
    }

    /**
     * @brief Registers a model in the reshaped set.
     * @param model Runtime model pointer.
     */
    void ModernM2::Remember(void* model)
    {
        std::unique_lock<std::shared_mutex> lock(reshapedMutex_);
        reshaped_.insert(model);
    }

    /**
     * @brief Drops a model from the reshaped set.
     * @param model Runtime model pointer.
     */
    void ModernM2::Forget(void* model)
    {
        std::unique_lock<std::shared_mutex> lock(reshapedMutex_);
        reshaped_.erase(model);
    }

    /**
     * @brief Queries whether a model is in the reshaped set.
     * @param model Runtime model pointer.
     * @return true if the model was reshaped by this module.
     */
    bool ModernM2::WasReshaped(void* model) const
    {
        std::shared_lock<std::shared_mutex> lock(reshapedMutex_);
        return reshaped_.find(model) != reshaped_.end();
    }

    // File-scope instance self-registers its handlers at DLL load via the EventScript ctor.
    ModernM2 g_modernM2;
}
