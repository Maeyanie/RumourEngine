#include "Text.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace RumourEngine::Text {

    namespace {
        std::atomic<unsigned> g_codePage{ 1252 };
        std::atomic<bool>     g_logged{ false };

        // The strings files' code pages, as xEdit reads them: Polish (and Czech translations)
        // Windows-1250, Russian Windows-1251, Japanese and Chinese UTF-8, the rest Windows-1252.
        // A plugin's own strings are whatever its author's tools wrote, which for a mod made for
        // that language is the same.
        unsigned CodePageFor(std::string lang) {
            for (auto& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lang == "polish" || lang == "czech") return 1250;
            if (lang == "russian") return 1251;
            return 1252;
        }
    }

    void Init() {
        std::string lang = "english";
        if (auto* s = RE::GetINISetting("sLanguage:General")) {
            if (const char* v = s->GetString(); v && *v) lang = v;
        }
        const auto cp = CodePageFor(lang);
        g_codePage = cp;
        logger::info("Text: game text that isn't UTF-8 is read as Windows-{} (sLanguage {})", cp, lang);
    }

    bool IsUtf8(std::string_view s) {
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        const auto* end = p + s.size();
        auto        cont = [](unsigned char c) { return (c & 0xC0) == 0x80; };
        while (p < end) {
            const unsigned char c = *p;
            if (c < 0x80) {
                ++p;
                continue;
            }
            // Unicode's table of well-formed sequences: no overlong forms, no surrogates, nothing
            // past U+10FFFF.
            std::size_t   n = 0;
            unsigned char lo = 0x80, hi = 0xBF;
            if (c >= 0xC2 && c <= 0xDF) n = 1;
            else if (c == 0xE0) n = 2, lo = 0xA0;
            else if (c == 0xED) n = 2, hi = 0x9F;
            else if (c >= 0xE1 && c <= 0xEF) n = 2;
            else if (c == 0xF0) n = 3, lo = 0x90;
            else if (c == 0xF4) n = 3, hi = 0x8F;
            else if (c >= 0xF1 && c <= 0xF3) n = 3;
            else return false;
            if (static_cast<std::size_t>(end - p) <= n) return false;
            if (p[1] < lo || p[1] > hi) return false;
            for (std::size_t k = 2; k <= n; ++k) {
                if (!cont(p[k])) return false;
            }
            p += n + 1;
        }
        return true;
    }

    std::string FromGame(std::string_view s) {
        if (s.empty() || IsUtf8(s)) return std::string(s);
        const auto cp = g_codePage.load();
        const int  len = static_cast<int>(std::min<std::size_t>(s.size(), INT_MAX));
        std::wstring wide(static_cast<std::size_t>(len), L'\0');
        const int    w = ::MultiByteToWideChar(cp, 0, s.data(), len, wide.data(), len);
        std::string  out;
        if (w > 0) {
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), w, nullptr, 0, nullptr, nullptr);
            out.resize(static_cast<std::size_t>(std::max(n, 0)));
            if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), w, out.data(), n, nullptr, nullptr);
        }
        if (out.empty()) {  // can't happen for these code pages, but never hand on the bad bytes
            out.assign(s);
            for (auto& c : out) {
                if (static_cast<unsigned char>(c) >= 0x80) c = '?';
            }
        }
        if (!g_logged.exchange(true)) {
            logger::info("Text: the first game text that wasn't UTF-8, read as Windows-{}: {}", cp, out.substr(0, 200));
        }
        return out;
    }

    std::string Dump(const nlohmann::json& j, int indent) {
        return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
    }
}
