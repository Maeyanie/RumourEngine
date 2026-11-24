#include "Runtime.h"

#include "AddressLibrary5.h"

#include <cstring>
#include <fstream>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <ShlObj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// The CommonLib RumourEngine builds on (CommonLibSSE NG 3.6.0, MIT) predates Skyrim 1.7: it takes 1.7 for
// SE, and reads only Address Library formats 1 and 2. Both are put right here, so that CommonLib stays as it
// is. Newer CommonLibs do this themselves, but they are GPL-3.0.
//
// Every AE address this CommonLib binds that 1.6.1170 has is also in the 1.7.99 and 1.7.104 tables, except
// TES::GetWaterHeight, which RumourEngine doesn't use (tools/check_addresses.py). Of the classes that 1.7.99
// rearranged, RumourEngine uses only PlayerCharacter, and only the Actor and reference parts that come before
// the new base class. That is why only those two 1.7 versions are let through: a later one needs the same check.
//
// On VR (1.4.15) CommonLib reads the VR Address Library itself, and stops the game if an ID it is asked for
// is missing. None that RumourEngine can reach is (checked against VR Address Library 0.158). What it gets
// wrong on VR is plugin numbering: it numbers light plugins as full ones, so forms are looked up by plugin
// here instead (FormIDIn, LookupForm).

namespace {

    // Four of CommonLib's private members. Access checks don't apply to the arguments of an explicit template
    // instantiation, which is what Access is for. If a CommonLib update renames them, this stops compiling
    // rather than going wrong.
    struct Key {
        friend void Publish(Key, const std::vector<std::uint32_t>& offsets);
    };

    template <auto RuntimeMember, auto Database, auto Initialized, auto TableMember>
    struct Access {
        friend void Publish(Key, const std::vector<std::uint32_t>& offsets) {
            using Table = std::remove_cvref_t<decltype((*Database).*TableMember)>;
            static std::vector<typename Table::value_type> mappings;  // the database's span points into this

            // One entry per ID, unused ones included (at offset 0), so that looking up an ID this version
            // lacks finds that entry, instead of quietly taking the next ID's address.
            mappings.resize(offsets.size());
            for (std::size_t id = 0; id < offsets.size(); ++id) mappings[id] = {id, offsets[id]};

            REL::Module::get().*RuntimeMember = REL::Module::Runtime::AE;  // 1.7 keeps the AE IDs and layouts
            (*Database).*TableMember = Table{mappings};
            Initialized->store(true);  // so that IDDatabase::get() doesn't go and load the file itself
        }
    };

    template struct Access<&REL::Module::_runtime, &REL::IDDatabase::_instance, &REL::IDDatabase::_initialized,
                           &REL::IDDatabase::_id2offset>;

    bool Checked17(const REL::Version& v) { return v[1] == 7 && (v[2] == 99 || v[2] == 104) && v[3] == 0; }

    std::optional<std::filesystem::path> FindLogDirectory() {
        // CommonLib 3.6.0's SKSE::log::log_directory() asks the game for the folder's name through an address
        // that 1.6.1130 and later don't have, so on 1.6.1170 it takes the next one, the string "Skyrim.INI",
        // and on 1.7 it would read nothing at all. The folder only depends on the edition, as SKSE's does.
        PWSTR      raw = nullptr;
        const auto result = ::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw);
        std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> documents(raw, ::CoTaskMemFree);
        if (FAILED(result) || !documents) return std::nullopt;

        std::wstring exe(MAX_PATH, L'\0');
        for (;;) {
            const auto n = ::GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
            if (n == 0) return std::nullopt;
            if (n < exe.size()) {
                exe.resize(n);
                break;
            }
            exe.resize(exe.size() * 2);
        }
        const std::filesystem::path game(exe);

        std::error_code ec;
        const wchar_t*  folder = L"Skyrim Special Edition GOG";
        if (::_wcsicmp(game.filename().c_str(), L"SkyrimVR.exe") == 0) {
            folder = L"Skyrim VR";
        } else if (std::filesystem::exists(game.parent_path() / L"steam_api64.dll", ec)) {
            folder = L"Skyrim Special Edition";
        }
        return std::filesystem::path(documents.get()) / L"My Games" / folder / L"SKSE";
    }
}

namespace RumourEngine::Runtime {

    std::optional<std::string> Prepare(const REL::Version& v) {
        const auto name = v.string("."sv);
        if (v[0] != 1) return "Skyrim " + name + " isn't a version RumourEngine knows";
        if (v[1] == 4 || v[1] == 5 || v[1] == 6) return std::nullopt;  // CommonLib reads these tables itself
        if (v[1] != 7) return "Skyrim " + name + " isn't a version RumourEngine knows";
        if (!Checked17(v)) return "RumourEngine hasn't been checked against Skyrim " + name + " yet";

        const auto    file = std::format("Data/SKSE/Plugins/versionlib-{}.bin", v.string("-"sv));
        std::ifstream in(file, std::ios::binary);
        const auto    table = AddressLibrary5::Read(in, {v[0], v[1], v[2], v[3]});
        if (table.error) return std::format("can't use the Address Library for Skyrim {} ({}): {}", name, file, table.error);

        Publish(Key{}, table.offsets);
        return std::nullopt;
    }

    std::optional<std::filesystem::path> LogDirectory() {
        static const auto dir = FindLogDirectory();  // the debug inbox asks on every poll
        return dir;
    }

    RE::FormID FormIDIn(const RE::TESFile* file, RE::FormID local) {
        if (!file) return 0;
        const auto slot = file->GetCompileIndex();
        if (slot == 0xFF) return 0;  // in the list, but not loaded
        // By the load slot, not the header's light flag: on VR without Skyrim VR ESL Support, a light-flagged
        // plugin loads as a full one. On SE and AE the two always agree.
        if (slot == 0xFE) return 0xFE000000u | (RE::FormID(file->GetSmallFileCompileIndex()) << 12) | (local & 0xFFF);
        return (RE::FormID(slot) << 24) | (local & 0xFFFFFF);
    }

    RE::TESForm* LookupForm(RE::FormID local, std::string_view plugin) {
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) return nullptr;
        for (const auto* file : handler->files) {
            const auto name = file ? file->GetFilename() : std::string_view{};
            if (name.size() != plugin.size() || ::_strnicmp(name.data(), plugin.data(), plugin.size()) != 0) continue;
            const auto id = FormIDIn(file, local);
            return id ? RE::TESForm::LookupByID(id) : nullptr;
        }
        return nullptr;
    }
}
