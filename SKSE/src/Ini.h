#pragma once

#include <map>

// mills.ini and its patches (ADR-001, "Overrides"). Another mod adds to or changes
// RumourEngine's mills by shipping SKSE/Plugins/RumourEngine/<Name>_mills.ini, the way
// SPID reads every *_DISTR.ini: mills.ini is read first, then each patch in file-name
// order, and a later line for the same key replaces an earlier one.

namespace RumourEngine::Ini {

    struct Entry {
        std::string key;    // lowercased
        std::string value;  // trimmed
        std::string file;   // the file it came from, for the log
    };

    struct Files {
        std::vector<std::string>                           names;     // in the order read
        std::map<std::string, std::vector<Entry>, std::less<>> sections;  // lowercased name -> its lines, every file's
    };

    /** <stem>.ini, then each *_<stem>.ini by file name, from Data/SKSE/Plugins/RumourEngine.
     *  `names` is empty when there is neither. */
    Files Read(std::string_view stem);

    /** mills.ini and its patches. */
    Files ReadMills();

    /** A section's lines, every file's, in the order read. */
    const std::vector<Entry>& Lines(const Files& files, std::string_view section);

    /** A section with one line per key, by key: a later line replaces an earlier one, and an
     *  empty value takes the key away. A line replacing another file's is logged. */
    std::vector<Entry> Keyed(const Files& files, std::string_view section);

    std::string Trim(std::string_view s);
    std::string Lower(std::string s);
}
