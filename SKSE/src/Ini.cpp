#include "Ini.h"

#include <algorithm>
#include <fstream>

namespace RumourEngine::Ini {

    namespace {
        std::string Utf8(const std::filesystem::path& p) {
            const auto s = p.u8string();
            return std::string(s.begin(), s.end());
        }

        void Read(const std::filesystem::path& path, const std::string& name, Files& out) {
            std::ifstream f(path, std::ios::binary);
            std::string   line, section;
            bool          first = true;
            while (f && std::getline(f, line)) {
                // Notepad and some editors start a UTF-8 file with a byte-order mark.
                if (first && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
                first = false;
                auto t = Trim(line);
                if (t.empty() || t[0] == ';' || t[0] == '#') continue;
                if (t.front() == '[' && t.back() == ']') {
                    section = Lower(Trim(std::string_view(t).substr(1, t.size() - 2)));
                    continue;
                }
                const auto eq = t.find('=');
                if (eq == std::string::npos || section.empty()) {
                    logger::warn("{}: \"{}\" isn't a key = value line in a section", name, t);
                    continue;
                }
                out.sections[section].push_back(
                    { Lower(Trim(std::string_view(t).substr(0, eq))), Trim(std::string_view(t).substr(eq + 1)), name });
            }
        }
    }

    std::string Trim(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return std::string(s);
    }

    std::string Lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    Files ReadMills() {
        auto out = Read("mills");
        if (std::find(out.names.begin(), out.names.end(), "mills.ini") == out.names.end()) {
            logger::warn("Mills: no mills.ini in Data/SKSE/Plugins/RumourEngine, so no faction mills, links or secrecy, and every "
                         "mill is described by its kind");
        }
        return out;
    }

    Files Read(std::string_view stem) {
        Files             out;
        const std::string main = std::string(stem) + ".ini";
        const std::string suffix = "_" + main;
        const auto        dir = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "RumourEngine";

        std::vector<std::pair<std::string, std::filesystem::path>> patches;  // by lowercased name
        std::error_code                                            ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fe;
            if (!it->is_regular_file(fe)) continue;
            auto name = Lower(Utf8(it->path().filename()));
            if (name.size() > suffix.size() && name.ends_with(suffix)) patches.emplace_back(std::move(name), it->path());
        }
        std::sort(patches.begin(), patches.end());

        if (std::error_code me; std::filesystem::is_regular_file(dir / main, me)) {
            Read(dir / main, main, out);
            out.names.push_back(main);
        }
        for (const auto& [lower, path] : patches) {
            const auto name = Utf8(path.filename());
            Read(path, name, out);
            out.names.push_back(name);
        }
        return out;
    }

    const std::vector<Entry>& Lines(const Files& files, std::string_view section) {
        static const std::vector<Entry> kNone;
        auto                            it = files.sections.find(section);
        return it != files.sections.end() ? it->second : kNone;
    }

    std::vector<Entry> Keyed(const Files& files, std::string_view section) {
        std::map<std::string, Entry> byKey;
        for (const auto& e : Lines(files, section)) {
            auto it = byKey.find(e.key);
            if (it != byKey.end() && it->second.file != e.file) {
                logger::info("{} [{}] {} {} {}'s", e.file, section, e.key, e.value.empty() ? "takes away" : "replaces",
                             it->second.file);
            }
            if (e.value.empty()) {
                if (it != byKey.end()) byKey.erase(it);
                continue;
            }
            byKey[e.key] = e;
        }
        std::vector<Entry> out;
        out.reserve(byKey.size());
        for (auto& [k, e] : byKey) out.push_back(std::move(e));
        return out;
    }
}
