#include "PluginScan.h"

#include <chrono>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <zlib.h>

namespace RumourEngine::PluginScan {

    namespace {

        constexpr std::uint32_t Sig(const char (&s)[5]) {
            return std::uint32_t(std::uint8_t(s[0])) | std::uint32_t(std::uint8_t(s[1])) << 8 |
                   std::uint32_t(std::uint8_t(s[2])) << 16 | std::uint32_t(std::uint8_t(s[3])) << 24;
        }

        constexpr auto kGRUP = Sig("GRUP");
        constexpr auto kTES4 = Sig("TES4");
        constexpr auto kMAST = Sig("MAST");
        constexpr auto kXXXX = Sig("XXXX");
        constexpr auto kEDID = Sig("EDID");
        constexpr auto kNPC_ = Sig("NPC_");
        constexpr auto kLCTN = Sig("LCTN");
        constexpr auto kFACT = Sig("FACT");
        constexpr auto kCELL = Sig("CELL");
        constexpr auto kWRLD = Sig("WRLD");
        constexpr auto kACHR = Sig("ACHR");
        constexpr auto kNAME = Sig("NAME");
        constexpr auto kDATA = Sig("DATA");
        constexpr auto kXLCN = Sig("XLCN");
        constexpr auto kXESP = Sig("XESP");
        constexpr auto kXCLC = Sig("XCLC");
        constexpr auto kMNAM = Sig("MNAM");
        constexpr auto kLCUN = Sig("LCUN");
        constexpr auto kACUN = Sig("ACUN");
        constexpr auto kQUST = Sig("QUST");
        constexpr auto kINDX = Sig("INDX");
        constexpr auto kQSDT = Sig("QSDT");
        constexpr auto kCNAM = Sig("CNAM");
        constexpr auto kQOBJ = Sig("QOBJ");
        constexpr auto kANAM = Sig("ANAM");

        // TES4 header flags.
        constexpr std::uint32_t kLocalized = 0x80;

        // Record flags.
        constexpr std::uint32_t kDeleted = 0x20;
        constexpr std::uint32_t kPersistent = 0x400;
        constexpr std::uint32_t kInitiallyDisabled = 0x800;
        constexpr std::uint32_t kCompressed = 0x40000;

        // Group types (the int32 at offset 12 of a GRUP header).
        constexpr std::int32_t kTopGroup = 0;
        constexpr std::int32_t kWorldChildren = 1;
        constexpr std::int32_t kCellChildren = 6;
        constexpr std::int32_t kCellPersistent = 8;
        constexpr std::int32_t kCellTemporary = 9;
        constexpr std::int32_t kCellVisibleDistant = 10;

        template <class T>
        T Read(const std::uint8_t* p) {
            T v;
            std::memcpy(&v, p, sizeof(T));
            return v;
        }

        /** A read-only view of a whole file. */
        class MappedFile {
        public:
            explicit MappedFile(const std::filesystem::path& path) {
                file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (file_ == INVALID_HANDLE_VALUE) return;
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(file_, &size) || size.QuadPart == 0) return;
                mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (!mapping_) return;
                data_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
                if (data_) size_ = static_cast<std::size_t>(size.QuadPart);
            }
            ~MappedFile() {
                if (data_) UnmapViewOfFile(data_);
                if (mapping_) CloseHandle(mapping_);
                if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
            }
            MappedFile(const MappedFile&) = delete;
            MappedFile& operator=(const MappedFile&) = delete;

            const std::uint8_t* data() const { return data_; }
            std::size_t         size() const { return size_; }

        private:
            HANDLE              file_ = INVALID_HANDLE_VALUE;
            HANDLE              mapping_ = nullptr;
            const std::uint8_t* data_ = nullptr;
            std::size_t         size_ = 0;
        };

        /** Calls fn(type, data, size) for each subrecord, following XXXX size overrides. */
        template <class F>
        void ForEachField(const std::uint8_t* p, std::size_t n, F&& fn) {
            std::size_t   i = 0;
            std::uint32_t bigSize = 0;
            while (i + 6 <= n) {
                const auto    type = Read<std::uint32_t>(p + i);
                std::uint32_t size = Read<std::uint16_t>(p + i + 4);
                i += 6;
                if (type == kXXXX && size == 4 && i + 4 <= n) {
                    bigSize = Read<std::uint32_t>(p + i);
                    i += 4;
                    continue;
                }
                if (bigSize) {
                    size = bigSize;
                    bigSize = 0;
                }
                if (i + size > n) return;
                fn(type, p + i, static_cast<std::size_t>(size));
                i += size;
            }
        }

        std::string ZString(const std::uint8_t* p, std::size_t n) {
            std::size_t len = 0;
            while (len < n && p[len]) ++len;
            return std::string(reinterpret_cast<const char*>(p), len);
        }

        std::string Lower(std::string_view s) {
            std::string out(s);
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        /** Reads one plugin into the shared result. */
        class FileScan {
        public:
            FileScan(Result& out, const std::unordered_map<std::string, const RE::TESFile*>& loaded,
                     const RE::TESFile* self) :
                out_(out), loaded_(loaded), self_(self) {}

            bool Run(const std::uint8_t* data, std::size_t size) {
                if (size < 24 || Read<std::uint32_t>(data) != kTES4) return false;
                const auto headerSize = Read<std::uint32_t>(data + 4);
                localized_ = (Read<std::uint32_t>(data + 8) & kLocalized) != 0;
                if (24 + std::size_t(headerSize) > size) return false;
                ForEachField(data + 24, headerSize, [&](std::uint32_t type, const std::uint8_t* p, std::size_t n) {
                    if (type == kMAST) {
                        auto it = loaded_.find(Lower(ZString(p, n)));
                        masters_.push_back(it != loaded_.end() ? it->second : nullptr);
                    }
                });
                Walk(data + 24 + headerSize, data + size, Context{});
                return true;
            }

        private:
            struct Context {
                RE::FormID world = 0;
                RE::FormID cell = 0;
            };

            /** A form ID as the game numbers it at runtime, or 0 if its file isn't loaded. */
            RE::FormID Resolve(std::uint32_t raw) const {
                if (raw == 0) return 0;
                const std::size_t idx = raw >> 24;
                const RE::TESFile* file = idx < masters_.size() ? masters_[idx] : self_;
                if (!file) return 0;
                if (file->IsLight()) {
                    return 0xFE000000u | (std::uint32_t(file->GetSmallFileCompileIndex()) << 12) | (raw & 0xFFF);
                }
                return (std::uint32_t(file->GetCompileIndex()) << 24) | (raw & 0xFFFFFF);
            }

            void Walk(const std::uint8_t* p, const std::uint8_t* end, Context ctx) {
                while (p + 24 <= end) {
                    const auto type = Read<std::uint32_t>(p);
                    const auto size = Read<std::uint32_t>(p + 4);
                    if (type == kGRUP) {
                        if (size < 24 || p + size > end) return;
                        const auto label = Read<std::uint32_t>(p + 8);
                        const auto gtype = Read<std::int32_t>(p + 12);
                        Context inner = ctx;
                        bool    enter = true;
                        switch (gtype) {
                        case kTopGroup:
                            enter = label == kNPC_ || label == kLCTN || label == kFACT || label == kCELL ||
                                    label == kWRLD || label == kQUST;
                            inner = Context{};
                            break;
                        case kWorldChildren:
                            inner.world = Resolve(label);
                            break;
                        case kCellChildren:
                        case kCellPersistent:
                        case kCellTemporary:
                        case kCellVisibleDistant:
                            inner.cell = Resolve(label);
                            break;
                        default:
                            break;
                        }
                        if (enter) Walk(p + 24, p + size, inner);
                        p += size;
                        continue;
                    }
                    const std::uint8_t* body = p + 24;
                    if (body + size > end) return;
                    Record(type, Read<std::uint32_t>(p + 8), Read<std::uint32_t>(p + 12), body, size, ctx);
                    p = body + size;
                }
            }

            void Record(std::uint32_t type, std::uint32_t flags, std::uint32_t raw, const std::uint8_t* data,
                        std::size_t size, const Context& ctx) {
                if (type != kNPC_ && type != kLCTN && type != kFACT && type != kCELL && type != kACHR && type != kQUST) return;
                const RE::FormID id = Resolve(raw);
                if (!id) return;

                if (flags & kDeleted) {
                    if (type == kACHR) out_.actors.erase(id);
                    if (type == kCELL) out_.cells.erase(id);
                    if (type == kQUST) out_.quests.erase(id);
                    return;
                }
                if (flags & kCompressed) {
                    if (size < 4) return;
                    const auto rawSize = Read<std::uint32_t>(data);
                    buffer_.resize(rawSize);
                    uLongf destLen = rawSize;
                    if (uncompress(buffer_.data(), &destLen, data + 4, static_cast<uLong>(size - 4)) != Z_OK) return;
                    data = buffer_.data();
                    size = destLen;
                }

                switch (type) {
                case kNPC_:
                case kFACT:
                case kLCTN:
                    {
                        auto& names = type == kNPC_ ? out_.npcEditorIDs
                                    : type == kFACT ? out_.factionEditorIDs
                                                    : out_.locationEditorIDs;
                        ForEachField(data, size, [&](std::uint32_t f, const std::uint8_t* p, std::size_t n) {
                            if (f == kEDID) names[id] = ZString(p, n);
                            if (type != kLCTN) return;
                            if (f == kMNAM && n >= 4) out_.locationMarkers[id] = Resolve(Read<std::uint32_t>(p));
                            if (f == kLCUN || f == kACUN) {
                                for (std::size_t i = 0; i + 12 <= n; i += 12) {
                                    const auto loc = Resolve(Read<std::uint32_t>(p + i + 8));
                                    out_.uniqueRows.insert({ Resolve(Read<std::uint32_t>(p + i)),
                                                             Resolve(Read<std::uint32_t>(p + i + 4)), loc ? loc : id });
                                }
                            }
                        });
                        break;
                    }
                case kCELL:
                    {
                        Cell  c;
                        auto  prev = out_.cells.find(id);
                        c.world = ctx.world;
                        if (!c.world && prev != out_.cells.end()) c.world = prev->second.world;
                        ForEachField(data, size, [&](std::uint32_t f, const std::uint8_t* p, std::size_t n) {
                            if (f == kDATA && n >= 1) c.interior = (p[0] & 0x01) != 0;
                            else if (f == kXCLC && n >= 8) {
                                c.x = Read<std::int32_t>(p);
                                c.y = Read<std::int32_t>(p + 4);
                                c.hasGrid = true;
                            } else if (f == kXLCN && n >= 4) c.location = Resolve(Read<std::uint32_t>(p));
                        });
                        if (c.interior) c.world = 0;
                        out_.cells[id] = c;
                        break;
                    }
                case kQUST:
                    {
                        // Stages come first: INDX, then each entry's QSDT, conditions and CNAM. The
                        // objectives (QOBJ) and aliases (ANAM on) follow, and nothing after them is
                        // a journal entry.
                        QuestJournal q;
                        q.file = std::string(self_->GetFilename());
                        q.localized = localized_;
                        LogEntry* entry = nullptr;
                        bool      stages = true;
                        bool      any = false;
                        ForEachField(data, size, [&](std::uint32_t f, const std::uint8_t* p, std::size_t n) {
                            if (!stages) return;
                            if (f == kQOBJ || f == kANAM) {
                                stages = false;
                            } else if (f == kINDX && n >= 2) {
                                q.stages.push_back({ Read<std::uint16_t>(p), {} });
                                entry = nullptr;
                            } else if (f == kQSDT && n >= 1 && !q.stages.empty()) {
                                entry = &q.stages.back().entries.emplace_back();
                                entry->flags = p[0];
                            } else if (f == kCNAM && entry) {
                                if (localized_ && n >= 4) entry->stringId = Read<std::uint32_t>(p);
                                else if (!localized_) entry->text = ZString(p, n);
                                any = any || entry->stringId || !entry->text.empty();
                            }
                        });
                        if (any) out_.quests[id] = std::move(q);
                        else out_.quests.erase(id);
                        break;
                    }
                case kACHR:
                    {
                        Placement a;
                        a.cell = ctx.cell;
                        a.disabled = (flags & kInitiallyDisabled) != 0;
                        a.persistent = (flags & kPersistent) != 0;
                        ForEachField(data, size, [&](std::uint32_t f, const std::uint8_t* p, std::size_t n) {
                            if (f == kNAME && n >= 4) a.base = Resolve(Read<std::uint32_t>(p));
                            else if (f == kDATA && n >= 12) std::memcpy(a.pos, p, 12);
                            else if (f == kXLCN && n >= 4) a.location = Resolve(Read<std::uint32_t>(p));
                            else if (f == kXESP && n >= 5) {
                                a.enableParent = Resolve(Read<std::uint32_t>(p));
                                a.opposite = (p[4] & 0x01) != 0;
                            }
                        });
                        out_.actors[id] = a;
                        break;
                    }
                default:
                    break;
                }
            }

            Result&                                                    out_;
            const std::unordered_map<std::string, const RE::TESFile*>& loaded_;
            const RE::TESFile*                                         self_;
            std::vector<const RE::TESFile*>                            masters_;
            std::vector<std::uint8_t>                                  buffer_;
            bool                                                       localized_ = false;
        };
    }

    std::shared_ptr<const Result> Run() {
        const auto start = std::chrono::steady_clock::now();
        auto       result = std::make_shared<Result>();

        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) return result;

        // Every active plugin, in load order. Inactive ones in the list keep
        // compile index 0xFF.
        std::vector<const RE::TESFile*>                       order;
        std::unordered_map<std::string, const RE::TESFile*> loaded;
        for (auto* file : handler->files) {
            if (!file || file->GetCompileIndex() == 0xFF) continue;
            order.push_back(file);
            loaded.emplace(Lower(file->GetFilename()), file);
        }

        const auto dataDir = std::filesystem::current_path() / "Data";
        for (const auto* file : order) {
            const auto name = std::string(file->GetFilename());
            // The game keeps file names in the system code page, which is
            // also what a narrow std::filesystem::path assumes on Windows.
            MappedFile view(dataDir / name);
            if (!view.data()) {
                logger::warn("Plugin scan: could not open {}", name);
                ++result->failedFiles;
                continue;
            }
            FileScan scan(*result, loaded, file);
            if (!scan.Run(view.data(), view.size())) {
                logger::warn("Plugin scan: {} is not a plugin this scanner can read", name);
                ++result->failedFiles;
                continue;
            }
            ++result->files;
            result->bytes += view.size();
        }

        result->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        logger::info("Plugin scan: {} plugins ({} MB) in {:.2f}s: {} placed actors, {} cells, {} NPC, {} location "
                     "and {} faction editor IDs, {} unique-NPC rows, {} quest journals{}",
                     result->files, result->bytes >> 20, result->seconds, result->actors.size(),
                     result->cells.size(), result->npcEditorIDs.size(), result->locationEditorIDs.size(),
                     result->factionEditorIDs.size(), result->uniqueRows.size(), result->quests.size(),
                     result->failedFiles ? fmt::format(" ({} unreadable)", result->failedFiles) : std::string{});
        return result;
    }
}
