#pragma once

namespace RumourEngine::Runtime {

    /** Whether RumourEngine can run on this game version, and on 1.7 the
     *  Address Library CommonLib can't read itself. Call it before SKSE::Init
     *  and before anything else that looks up a game address. Returns nothing
     *  when the game can be run, otherwise why not, having changed nothing. */
    std::optional<std::string> Prepare(const REL::Version& game);

    /** Documents/My Games/<the game's folder>/SKSE, where SKSE keeps its logs.
     *  Worked out without the game's addresses, so it's safe before Prepare. */
    std::optional<std::filesystem::path> LogDirectory();

    /** The runtime form ID of a form in a loaded plugin, from its ID in that
     *  plugin (any index byte it carries is ignored), or 0 if the plugin isn't
     *  loaded. Light plugins get 0xFE plus their light slot, as SE and AE
     *  number them, and as VR does with Skyrim VR ESL Support. */
    RE::FormID FormIDIn(const RE::TESFile* file, RE::FormID local);

    /** A form by its plugin's file name and its ID in that plugin, or nullptr.
     *  Use it instead of TESDataHandler::LookupForm, which in CommonLib 3.6.0
     *  numbers light plugins as full ones on VR. */
    RE::TESForm* LookupForm(RE::FormID local, std::string_view plugin);

    template <class T>
    T* LookupForm(RE::FormID local, std::string_view plugin) {
        auto* form = LookupForm(local, plugin);
        return form ? form->As<T>() : nullptr;
    }
}
