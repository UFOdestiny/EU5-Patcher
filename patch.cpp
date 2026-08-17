/**
 * EU5 Patcher - Enable Achievements Unconditionally
 *
 * Location strategy:
 *   CanGetAchievements string -> registration xref -> callback -> predicate
 *   Branch-2 -> checksum callee
 *   IsGameRuleEnabled string -> registration xref -> callback
 *
 * Patch #1/#2/#3/#4 all replace the target function entry with:
 *   mov eax, 1
 *   ret
 *
 * Compile (MSVC): cl /std:c++17 /O2 /EHsc patch.cpp
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "Advapi32.lib")
#endif

namespace fs = std::filesystem;

namespace
{
    constexpr std::string_view CAN_GET_ACHIEVEMENTS_ANCHOR = "CanGetAchievements";
    constexpr std::string_view IS_GAME_RULE_ENABLED_ANCHOR = "IsGameRuleEnabled";

    constexpr std::array<uint8_t, 6> RETURN_TRUE = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};

    constexpr std::array<uint32_t, 6> CHECKSUM_STATE_OFFSETS = {
        0x130, 0x131, 0x132, 0x133, 0x134, 0x139};
    constexpr size_t CHECKSUM_MIN_STATE_FIELDS = 4;
    constexpr size_t CHECKSUM_PATCHED_MIN_STATE_FIELDS = 3;

    constexpr std::string_view EU5_PATH = "eu5.exe";
    constexpr std::string_view GAME_FOLDER = "Europa Universalis V";
    constexpr std::string_view APP_ID = "3450310";
    constexpr std::string_view EU5_BACKUP_SUFFIX = ".backup";

    bool debug_info = false;

    struct PatchError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    struct PESection
    {
        std::string name;
        uint32_t rva{};
        uint32_t virtual_size{};
        uint32_t raw_offset{};
        uint32_t raw_size{};

        [[nodiscard]] uint64_t raw_rva_end() const
        {
            return static_cast<uint64_t>(rva) + raw_size;
        }

        [[nodiscard]] bool contains_rva(uint64_t value) const
        {
            const uint64_t size = std::max<uint64_t>(virtual_size, raw_size);
            return value >= rva && value < static_cast<uint64_t>(rva) + size;
        }

        [[nodiscard]] bool contains_raw_offset(size_t value) const
        {
            return value >= raw_offset &&
                   value < static_cast<uint64_t>(raw_offset) + raw_size;
        }
    };

    struct RuntimeFunctionRange
    {
        uint32_t begin{};
        uint32_t end{};
    };

    struct RipTarget
    {
        uint32_t instruction_rva{};
        uint32_t target_rva{};
    };

    struct Rel32Target
    {
        uint32_t instruction_rva{};
        uint32_t target_rva{};
        uint8_t opcode{};
    };

    struct ChecksumScore
    {
        size_t score{};
        std::set<uint32_t> fields;
    };

    struct PatchJob
    {
        std::string label;
        size_t offset{};
        uint32_t rva{};
    };

    [[nodiscard]] uint16_t read_u16(const std::vector<uint8_t> &data, size_t offset)
    {
        if (offset + sizeof(uint16_t) > data.size())
            throw PatchError("Unexpected end of file while reading PE data.");

        uint16_t value{};
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] uint32_t read_u32(const std::vector<uint8_t> &data, size_t offset)
    {
        if (offset + sizeof(uint32_t) > data.size())
            throw PatchError("Unexpected end of file while reading PE data.");

        uint32_t value{};
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] int32_t read_i32(const std::vector<uint8_t> &data, size_t offset)
    {
        if (offset + sizeof(int32_t) > data.size())
            throw PatchError("Unexpected end of file while reading x64 displacement.");

        int32_t value{};
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] bool is_checksum_state_offset(uint32_t value)
    {
        return std::find(CHECKSUM_STATE_OFFSETS.begin(),
                         CHECKSUM_STATE_OFFSETS.end(),
                         value) != CHECKSUM_STATE_OFFSETS.end();
    }

    [[nodiscard]] std::optional<std::vector<uint8_t>> read_file(const fs::path &filepath)
    {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file)
            return std::nullopt;

        const auto size = file.tellg();
        if (size < 0)
            return std::nullopt;

        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(static_cast<size_t>(size));
        if (!buffer.empty() &&
            !file.read(reinterpret_cast<char *>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size())))
        {
            return std::nullopt;
        }

        return buffer;
    }

    [[nodiscard]] bool write_file(const fs::path &filepath,
                                  const std::vector<uint8_t> &data)
    {
        std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        if (!data.empty())
        {
            file.write(reinterpret_cast<const char *>(data.data()),
                       static_cast<std::streamsize>(data.size()));
        }
        return file.good();
    }

    [[nodiscard]] bool create_backup(const fs::path &source, const fs::path &dest)
    {
        std::error_code ec;
        fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

#ifdef _WIN32
    [[nodiscard]] std::optional<std::wstring> read_registry_string(
        HKEY root, const wchar_t *subkey, const wchar_t *value_name)
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
            return std::nullopt;

        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(hKey, value_name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
            type != REG_SZ)
        {
            RegCloseKey(hKey);
            return std::nullopt;
        }

        std::wstring buffer(size / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(hKey, value_name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buffer.data()), &size) != ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return std::nullopt;
        }

        RegCloseKey(hKey);
        while (!buffer.empty() && buffer.back() == L'\0')
            buffer.pop_back();

        return buffer;
    }
#endif

    [[nodiscard]] std::optional<fs::path> get_steam_install_path()
    {
#ifdef _WIN32
        if (auto primary = read_registry_string(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                L"InstallPath"))
        {
            return fs::path(*primary);
        }

        if (auto fallback = read_registry_string(
                HKEY_CURRENT_USER,
                L"Software\\Valve\\Steam",
                L"SteamPath"))
        {
            return fs::path(*fallback);
        }
#else
        const char *home = std::getenv("HOME");
        if (home)
        {
            const std::vector<fs::path> candidates = {
                fs::path(home) / ".local/share/Steam",
                fs::path(home) / ".steam/steam",
                fs::path(home) / ".var/app/com.valvesoftware.Steam/.steam/steam",
            };

            for (const auto &p : candidates)
            {
                if (fs::is_directory(p))
                    return p;
            }
        }
#endif
        return std::nullopt;
    }

    [[nodiscard]] std::string trim(const std::string &s)
    {
        const auto first = s.find_first_not_of(" \t\n\r");
        if (first == std::string::npos)
            return {};

        const auto last = s.find_last_not_of(" \t\n\r");
        return s.substr(first, last - first + 1);
    }

    [[nodiscard]] std::vector<std::string> extract_quoted_tokens(const std::string &line)
    {
        std::vector<std::string> tokens;
        size_t pos = 0;

        while (true)
        {
            const auto start = line.find('"', pos);
            if (start == std::string::npos)
                break;

            const auto end = line.find('"', start + 1);
            if (end == std::string::npos)
                break;

            tokens.push_back(line.substr(start + 1, end - start - 1));
            pos = end + 1;
        }

        return tokens;
    }

    [[nodiscard]] std::vector<fs::path> find_all_steam_libraries_with_app(
        const fs::path &vdf_path,
        std::string_view target_appid)
    {
        std::vector<fs::path> results;
        std::ifstream file(vdf_path);
        if (!file)
            return results;

        std::string current_path;
        bool in_apps_block = false;
        std::string line;

        while (std::getline(file, line))
        {
            const auto trimmed = trim(line);
            const auto tokens = extract_quoted_tokens(trimmed);

            if (tokens.empty())
            {
                if (in_apps_block && trimmed == "}")
                    in_apps_block = false;
                continue;
            }

            if (!in_apps_block)
            {
                if (tokens[0] == "path" && tokens.size() >= 2)
                {
                    current_path = tokens[1];
                    continue;
                }

                if (tokens[0] == "apps")
                    in_apps_block = true;

                continue;
            }

            if (tokens[0] == target_appid && !current_path.empty())
                results.emplace_back(current_path);
        }

        return results;
    }

    [[nodiscard]] std::optional<fs::path> get_game_folder(std::string_view name)
    {
        const auto steam_path = get_steam_install_path();
        if (!steam_path)
            return std::nullopt;

        const auto library_db = *steam_path / "steamapps" / "libraryfolders.vdf";
        const auto library_paths = find_all_steam_libraries_with_app(library_db, APP_ID);

        for (const auto &library_path : library_paths)
        {
            const auto game_folder =
                library_path / "steamapps" / "common" / std::string(name);
            const auto binary = game_folder / "binaries" / EU5_PATH;
            if (fs::is_regular_file(binary))
                return game_folder;
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<fs::path> locate_eu5()
    {
        const fs::path local_path{EU5_PATH};
        if (fs::exists(local_path))
            return local_path;

        if (const auto game_folder = get_game_folder(GAME_FOLDER))
        {
            const auto steam_path = *game_folder / "binaries" / EU5_PATH;
            if (fs::exists(steam_path))
                return steam_path;
        }

        return std::nullopt;
    }

    [[nodiscard]] std::vector<PESection> parse_pe_sections(const std::vector<uint8_t> &data)
    {
        if (data.size() < 0x40 || data[0] != 'M' || data[1] != 'Z')
            throw PatchError("Target is not a valid PE image (missing MZ header).");

        const uint32_t pe_offset = read_u32(data, 0x3C);
        if (static_cast<uint64_t>(pe_offset) + 24 > data.size() ||
            data[pe_offset] != 'P' || data[pe_offset + 1] != 'E' ||
            data[pe_offset + 2] != 0 || data[pe_offset + 3] != 0)
        {
            throw PatchError("Target is not a valid PE image (missing PE header).");
        }

        const uint16_t number_of_sections = read_u16(data, pe_offset + 6);
        const uint16_t optional_header_size = read_u16(data, pe_offset + 20);
        const uint64_t section_table =
            static_cast<uint64_t>(pe_offset) + 24 + optional_header_size;

        std::vector<PESection> sections;
        sections.reserve(number_of_sections);

        for (uint16_t i = 0; i < number_of_sections; ++i)
        {
            const uint64_t off64 = section_table + static_cast<uint64_t>(i) * 40;
            if (off64 + 40 > data.size())
                throw PatchError("PE section table is truncated.");

            const size_t off = static_cast<size_t>(off64);
            char name_buf[9]{};
            std::memcpy(name_buf, data.data() + off, 8);

            PESection section;
            section.name = name_buf;
            section.virtual_size = read_u32(data, off + 8);
            section.rva = read_u32(data, off + 12);
            section.raw_size = read_u32(data, off + 16);
            section.raw_offset = read_u32(data, off + 20);
            sections.push_back(std::move(section));
        }

        return sections;
    }

    [[nodiscard]] const PESection &get_section(const std::vector<PESection> &sections,
                                               std::string_view name)
    {
        const auto it = std::find_if(
            sections.begin(), sections.end(),
            [name](const PESection &section)
            { return section.name == name; });

        if (it == sections.end())
            throw PatchError("PE section '" + std::string(name) + "' was not found.");

        return *it;
    }

    [[nodiscard]] std::optional<std::reference_wrapper<const PESection>>
    try_get_section(const std::vector<PESection> &sections, std::string_view name)
    {
        const auto it = std::find_if(
            sections.begin(), sections.end(),
            [name](const PESection &section)
            { return section.name == name; });

        if (it == sections.end())
            return std::nullopt;
        return std::cref(*it);
    }

    [[nodiscard]] uint32_t raw_offset_to_rva(const std::vector<PESection> &sections,
                                             size_t offset)
    {
        for (const auto &section : sections)
        {
            if (section.contains_raw_offset(offset))
            {
                const uint64_t rva = static_cast<uint64_t>(section.rva) +
                                     (offset - section.raw_offset);
                if (rva > UINT32_MAX)
                    break;
                return static_cast<uint32_t>(rva);
            }
        }

        std::ostringstream oss;
        oss << "File offset 0x" << std::hex << offset
            << " does not belong to a PE section.";
        throw PatchError(oss.str());
    }

    [[nodiscard]] size_t rva_to_raw_offset(const std::vector<PESection> &sections,
                                           uint32_t rva)
    {
        for (const auto &section : sections)
        {
            if (rva >= section.rva)
            {
                const uint64_t delta = static_cast<uint64_t>(rva) - section.rva;
                if (delta < section.raw_size)
                    return static_cast<size_t>(section.raw_offset + delta);
            }
        }

        std::ostringstream oss;
        oss << "RVA 0x" << std::hex << rva << " does not map to raw file data.";
        throw PatchError(oss.str());
    }

    [[nodiscard]] std::vector<uint32_t> find_string_rvas(
        const std::vector<uint8_t> &data,
        const std::vector<PESection> &sections,
        std::string_view anchor)
    {
        std::vector<uint8_t> needle(anchor.begin(), anchor.end());
        needle.push_back(0);

        std::vector<uint32_t> rvas;
        auto begin = data.begin();

        while (begin != data.end())
        {
            const auto it = std::search(begin, data.end(), needle.begin(), needle.end());
            if (it == data.end())
                break;

            const size_t offset = static_cast<size_t>(std::distance(data.begin(), it));
            try
            {
                rvas.push_back(raw_offset_to_rva(sections, offset));
            }
            catch (const PatchError &)
            {
                // Ignore matching bytes outside mapped PE sections.
            }

            begin = it + 1;
        }

        if (rvas.empty())
            throw PatchError("String anchor '" + std::string(anchor) + "' was not found.");

        return rvas;
    }

    [[nodiscard]] std::vector<RipTarget> iter_rip_lea_targets(
        const std::vector<uint8_t> &data,
        const PESection &text,
        const std::array<uint8_t, 3> &opcode,
        uint32_t start_rva,
        uint32_t end_rva)
    {
        const uint64_t clipped_start = std::max<uint64_t>(start_rva, text.rva);
        const uint64_t clipped_end = std::min<uint64_t>(end_rva, text.raw_rva_end());

        std::vector<RipTarget> results;
        if (clipped_start >= clipped_end)
            return results;

        size_t start_off = text.raw_offset +
                           static_cast<size_t>(clipped_start - text.rva);
        size_t end_off = text.raw_offset +
                         static_cast<size_t>(clipped_end - text.rva);
        end_off = std::min(end_off, data.size());

        for (size_t pos = start_off; pos + 7 <= end_off; ++pos)
        {
            if (data[pos] != opcode[0] || data[pos + 1] != opcode[1] ||
                data[pos + 2] != opcode[2])
            {
                continue;
            }

            const int32_t disp = read_i32(data, pos + 3);
            const uint64_t insn_rva64 = static_cast<uint64_t>(text.rva) +
                                        (pos - text.raw_offset);
            const int64_t target = static_cast<int64_t>(insn_rva64) + 7 + disp;
            if (target < 0 || target > UINT32_MAX)
                continue;

            results.push_back({static_cast<uint32_t>(insn_rva64),
                               static_cast<uint32_t>(target)});
        }

        return results;
    }

    [[nodiscard]] std::vector<RuntimeFunctionRange> parse_runtime_function_ranges(
        const std::vector<uint8_t> &data,
        const std::vector<PESection> &sections)
    {
        const auto pdata_opt = try_get_section(sections, ".pdata");
        if (!pdata_opt)
            return {};

        const auto &pdata = pdata_opt->get();
        const size_t section_end = std::min<size_t>(
            data.size(), static_cast<uint64_t>(pdata.raw_offset) + pdata.raw_size);

        std::vector<RuntimeFunctionRange> ranges;
        for (size_t off = pdata.raw_offset; off + 12 <= section_end; off += 12)
        {
            const uint32_t begin = read_u32(data, off);
            const uint32_t end = read_u32(data, off + 4);
            if (begin != 0 && end > begin)
                ranges.push_back({begin, end});
        }

        return ranges;
    }

    [[nodiscard]] RuntimeFunctionRange function_range_for_rva(
        uint32_t rva,
        const PESection &text,
        const std::vector<RuntimeFunctionRange> &runtime_ranges,
        uint32_t fallback_before = 0x20,
        uint32_t fallback_after = 0x100)
    {
        std::optional<RuntimeFunctionRange> best;

        for (const auto &range : runtime_ranges)
        {
            if (range.begin <= rva && rva < range.end)
            {
                if (!best || (range.end - range.begin) < (best->end - best->begin))
                    best = range;
            }
        }

        if (best)
            return *best;

        const uint64_t begin =
            rva > fallback_before ? static_cast<uint64_t>(rva) - fallback_before : text.rva;
        const uint64_t end = static_cast<uint64_t>(rva) + fallback_after;

        return {
            static_cast<uint32_t>(std::max<uint64_t>(text.rva, begin)),
            static_cast<uint32_t>(std::min<uint64_t>(text.raw_rva_end(), end))};
    }

    [[nodiscard]] std::vector<RipTarget> find_anchor_xrefs(
        const std::vector<uint8_t> &data,
        const PESection &text,
        const std::vector<uint32_t> &anchor_rvas)
    {
        const std::unordered_set<uint32_t> anchors(anchor_rvas.begin(), anchor_rvas.end());
        std::vector<RipTarget> results;

        // lea rdx, [rip + disp32]
        for (const auto &item : iter_rip_lea_targets(
                 data, text, {0x48, 0x8D, 0x15}, text.rva,
                 static_cast<uint32_t>(text.raw_rva_end())))
        {
            if (anchors.count(item.target_rva))
                results.push_back(item);
        }

        return results;
    }

    [[nodiscard]] uint32_t find_registered_callback(
        const std::vector<uint8_t> &data,
        const PESection &text,
        const std::vector<RuntimeFunctionRange> &runtime_ranges,
        uint32_t string_xref_rva,
        uint32_t string_rva)
    {
        const auto range = function_range_for_rva(
            string_xref_rva, text, runtime_ranges, 0x20, 0x180);

        std::vector<RipTarget> candidates;
        for (const auto &item : iter_rip_lea_targets(
                 data, text, {0x48, 0x8D, 0x15}, string_xref_rva + 7, range.end))
        {
            if (item.target_rva != string_rva && text.contains_rva(item.target_rva))
                candidates.push_back(item);
        }

        if (candidates.empty())
        {
            std::ostringstream oss;
            oss << "Could not resolve registered callback after string xref at RVA 0x"
                << std::hex << string_xref_rva << '.';
            throw PatchError(oss.str());
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const RipTarget &a, const RipTarget &b)
                  {
                      return a.instruction_rva < b.instruction_rva;
                  });

        if (debug_info && candidates.size() > 1)
        {
            std::cout << "Warning: multiple callback candidates after RVA 0x"
                      << std::hex << string_xref_rva << ": ";
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                if (i)
                    std::cout << ", ";
                std::cout << "0x" << candidates[i].target_rva;
            }
            std::cout << std::dec << '\n';
        }

        return candidates.front().target_rva;
    }

    [[nodiscard]] uint32_t resolve_can_get_achievements_predicate(
        const std::vector<uint8_t> &data,
        const PESection &text,
        const std::vector<RuntimeFunctionRange> &runtime_ranges,
        uint32_t callback_rva)
    {
        const auto range = function_range_for_rva(
            callback_rva, text, runtime_ranges, 0, 0x60);

        std::vector<RipTarget> candidates;
        // lea rcx, [rip + disp32]
        for (const auto &item : iter_rip_lea_targets(
                 data, text, {0x48, 0x8D, 0x0D}, callback_rva, range.end))
        {
            if (text.contains_rva(item.target_rva))
                candidates.push_back(item);
        }

        if (candidates.empty())
        {
            std::ostringstream oss;
            oss << "Could not resolve inner CanGetAchievements predicate from callback RVA 0x"
                << std::hex << callback_rva << '.';
            throw PatchError(oss.str());
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const RipTarget &a, const RipTarget &b)
                  {
                      return a.instruction_rva < b.instruction_rva;
                  });

        if (debug_info && candidates.size() > 1)
        {
            std::cout << "Warning: multiple predicate candidates in callback RVA 0x"
                      << std::hex << callback_rva << ": ";
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                if (i)
                    std::cout << ", ";
                std::cout << "0x" << candidates[i].target_rva;
            }
            std::cout << std::dec << '\n';
        }

        return candidates.front().target_rva;
    }

    [[nodiscard]] std::vector<Rel32Target> iter_rel32_control_targets(
        const std::vector<uint8_t> &data,
        const PESection &text,
        uint32_t start_rva,
        uint32_t end_rva)
    {
        const uint64_t clipped_start = std::max<uint64_t>(start_rva, text.rva);
        const uint64_t clipped_end = std::min<uint64_t>(end_rva, text.raw_rva_end());

        std::vector<Rel32Target> results;
        if (clipped_start >= clipped_end)
            return results;

        size_t start_off = text.raw_offset +
                           static_cast<size_t>(clipped_start - text.rva);
        size_t end_off = text.raw_offset +
                         static_cast<size_t>(clipped_end - text.rva);
        end_off = std::min(end_off, data.size());

        for (size_t pos = start_off; pos + 5 <= end_off; ++pos)
        {
            const uint8_t opcode = data[pos];
            if (opcode != 0xE8 && opcode != 0xE9)
                continue;

            const int32_t disp = read_i32(data, pos + 1);
            const uint64_t insn_rva64 = static_cast<uint64_t>(text.rva) +
                                        (pos - text.raw_offset);
            const int64_t target = static_cast<int64_t>(insn_rva64) + 5 + disp;
            if (target < 0 || target > UINT32_MAX)
                continue;

            const auto target_rva = static_cast<uint32_t>(target);
            if (text.contains_rva(target_rva))
            {
                results.push_back({static_cast<uint32_t>(insn_rva64),
                                   target_rva,
                                   opcode});
            }
        }

        return results;
    }

    [[nodiscard]] bool is_return_true_at_rva(
        const std::vector<uint8_t> &data,
        const std::vector<PESection> &sections,
        uint32_t target_rva)
    {
        try
        {
            const size_t offset = rva_to_raw_offset(sections, target_rva);
            if (offset + RETURN_TRUE.size() > data.size())
                return false;

            return std::equal(RETURN_TRUE.begin(), RETURN_TRUE.end(), data.begin() + offset);
        }
        catch (const PatchError &)
        {
            return false;
        }
    }

    [[nodiscard]] ChecksumScore checksum_structure_score(
        const std::vector<uint8_t> &data,
        const std::vector<PESection> &sections,
        const PESection &text,
        const std::vector<RuntimeFunctionRange> &runtime_ranges,
        uint32_t candidate_rva)
    {
        const auto range = function_range_for_rva(
            candidate_rva, text, runtime_ranges, 0, 0x180);

        size_t start_off{};
        size_t end_off{};
        try
        {
            start_off = rva_to_raw_offset(sections, range.begin);
            if (range.end <= range.begin)
                return {};
            end_off = rva_to_raw_offset(sections, range.end - 1) + 1;
        }
        catch (const PatchError &)
        {
            return {};
        }

        end_off = std::min(end_off, data.size());
        std::set<uint32_t> found;

        // Match local instances of: 80 /7 [base+disp32], 00
        // and score accesses to the checksum state neighborhood.
        for (size_t pos = start_off; pos + 7 <= end_off; ++pos)
        {
            if (data[pos] != 0x80)
                continue;

            const uint8_t modrm = data[pos + 1];
            const uint8_t mod = (modrm >> 6) & 0x3;
            const uint8_t reg = (modrm >> 3) & 0x7;
            if (mod != 0x2 || reg != 0x7 || data[pos + 6] != 0)
                continue;

            const uint32_t disp = read_u32(data, pos + 2);
            if (is_checksum_state_offset(disp))
                found.insert(disp);
        }

        return {found.size(), std::move(found)};
    }

    [[nodiscard]] std::pair<uint32_t, uint32_t> resolve_branch2_and_checksum(
        const std::vector<uint8_t> &data,
        const std::vector<PESection> &sections,
        const PESection &text,
        const std::vector<RuntimeFunctionRange> &runtime_ranges,
        const std::vector<uint32_t> &predicates)
    {
        std::unordered_set<uint32_t> runtime_starts;
        for (const auto &range : runtime_ranges)
            runtime_starts.insert(range.begin);

        struct Match
        {
            uint32_t branch_rva{};
            uint32_t checksum_rva{};
            size_t score{};
            std::set<uint32_t> fields;
            uint32_t instruction_rva{};
        };

        std::vector<Match> matches;

        for (const uint32_t predicate_rva : predicates)
        {
            const auto range = function_range_for_rva(
                predicate_rva, text, runtime_ranges, 0, 0x220);

            std::unordered_set<uint32_t> seen_targets;
            for (const auto &item : iter_rel32_control_targets(
                     data, text, range.begin, range.end))
            {
                if (item.target_rva == predicate_rva ||
                    !seen_targets.insert(item.target_rva).second)
                {
                    continue;
                }

                if (!runtime_starts.empty() && !runtime_starts.count(item.target_rva))
                    continue;

                const auto score = checksum_structure_score(
                    data, sections, text, runtime_ranges, item.target_rva);

                const size_t min_score =
                    is_return_true_at_rva(data, sections, item.target_rva)
                        ? CHECKSUM_PATCHED_MIN_STATE_FIELDS
                        : CHECKSUM_MIN_STATE_FIELDS;

                if (score.score >= min_score)
                {
                    matches.push_back({predicate_rva,
                                       item.target_rva,
                                       score.score,
                                       score.fields,
                                       item.instruction_rva});
                }
            }
        }

        if (debug_info)
        {
            for (const auto &match : matches)
            {
                std::cout << "Checksum candidate: branch=0x" << std::hex
                          << match.branch_rva << ", call/jmp@0x"
                          << match.instruction_rva << " -> 0x"
                          << match.checksum_rva << ", score=" << std::dec
                          << match.score << ", fields=";

                bool first = true;
                for (const auto field : match.fields)
                {
                    if (!first)
                        std::cout << ',';
                    std::cout << "0x" << std::hex << field;
                    first = false;
                }
                std::cout << std::dec << '\n';
            }
        }

        using Pair = std::pair<uint32_t, uint32_t>;
        std::map<Pair, Match> unique_pairs;
        for (const auto &match : matches)
        {
            const Pair key{match.branch_rva, match.checksum_rva};
            const auto it = unique_pairs.find(key);
            if (it == unique_pairs.end() || match.score > it->second.score)
                unique_pairs[key] = match;
        }

        if (unique_pairs.size() != 1)
        {
            std::ostringstream oss;
            oss << "Could not uniquely resolve Branch-2 -> checksum from local call structure; candidates: ";

            if (unique_pairs.empty())
            {
                oss << "none";
            }
            else
            {
                bool first = true;
                for (const auto &[key, value] : unique_pairs)
                {
                    (void)value;
                    if (!first)
                        oss << ", ";
                    oss << "branch 0x" << std::hex << key.first
                        << " -> checksum 0x" << key.second;
                    first = false;
                }
            }
            oss << '.';
            throw PatchError(oss.str());
        }

        return unique_pairs.begin()->first;
    }

    void add_return_true_job(std::vector<PatchJob> &jobs,
                             const std::vector<uint8_t> &data,
                             const std::vector<PESection> &sections,
                             std::string label,
                             uint32_t target_rva)
    {
        const size_t offset = rva_to_raw_offset(sections, target_rva);
        if (offset + RETURN_TRUE.size() > data.size())
            throw PatchError("Patch target extends past the end of the file.");

        if (is_return_true_at_rva(data, sections, target_rva))
        {
            std::cout << label << ": already patched successfully at RVA 0x"
                      << std::hex << target_rva << std::dec << ".\n";
            return;
        }

        jobs.push_back({std::move(label), offset, target_rva});
    }

    [[nodiscard]] std::vector<PatchJob> prepare_patch_jobs(
        const std::vector<uint8_t> &data,
        const std::vector<PESection> &sections)
    {
        const auto &text = get_section(sections, ".text");
        const auto runtime_ranges = parse_runtime_function_ranges(data, sections);
        std::vector<PatchJob> jobs;

        // CanGetAchievements -> two registered callbacks -> two predicates.
        const auto can_rvas = find_string_rvas(
            data, sections, CAN_GET_ACHIEVEMENTS_ANCHOR);
        const auto can_xrefs = find_anchor_xrefs(data, text, can_rvas);

        if (can_xrefs.size() < 2)
        {
            std::ostringstream oss;
            oss << "Expected at least 2 CanGetAchievements registrations, found "
                << can_xrefs.size() << '.';
            throw PatchError(oss.str());
        }

        std::vector<uint32_t> predicates;
        for (const auto &xref : can_xrefs)
        {
            const uint32_t callback_rva = find_registered_callback(
                data, text, runtime_ranges, xref.instruction_rva, xref.target_rva);
            const uint32_t predicate_rva = resolve_can_get_achievements_predicate(
                data, text, runtime_ranges, callback_rva);

            if (std::find(predicates.begin(), predicates.end(), predicate_rva) == predicates.end())
                predicates.push_back(predicate_rva);
        }

        if (predicates.size() != 2)
        {
            std::ostringstream oss;
            oss << "Expected exactly 2 distinct CanGetAchievements predicates, found "
                << predicates.size() << '.';
            throw PatchError(oss.str());
        }

        // Identify Branch-2 semantically from its checksum call.
        const auto [branch2_rva, checksum_rva] = resolve_branch2_and_checksum(
            data, sections, text, runtime_ranges, predicates);

        std::vector<uint32_t> branch1_candidates;
        for (const auto rva : predicates)
        {
            if (rva != branch2_rva)
                branch1_candidates.push_back(rva);
        }

        if (branch1_candidates.size() != 1)
            throw PatchError("Could not uniquely determine CanGetAchievements Branch-1.");

        const uint32_t branch1_rva = branch1_candidates.front();

        if (debug_info)
        {
            std::cout << "CanGetAchievements Branch-1: RVA 0x" << std::hex
                      << branch1_rva << '\n';
            std::cout << "CanGetAchievements Branch-2: RVA 0x"
                      << branch2_rva << '\n';
            std::cout << "Branch-2 checksum: RVA 0x"
                      << checksum_rva << std::dec << '\n';
        }

        // All addresses above were resolved from the untouched image.
        add_return_true_job(jobs, data, sections,
                            "Patch #1 (CanGetAchievements Branch-1)", branch1_rva);
        add_return_true_job(jobs, data, sections,
                            "Patch #3 (checksum via Branch-2)", checksum_rva);
        add_return_true_job(jobs, data, sections,
                            "Patch #2 (CanGetAchievements Branch-2)", branch2_rva);

        // IsGameRuleEnabled -> registered callback.
        const auto rule_rvas = find_string_rvas(
            data, sections, IS_GAME_RULE_ENABLED_ANCHOR);
        const auto rule_xrefs = find_anchor_xrefs(data, text, rule_rvas);
        if (rule_xrefs.empty())
            throw PatchError("No IsGameRuleEnabled registration xref was found.");

        std::vector<uint32_t> callbacks;
        for (const auto &xref : rule_xrefs)
        {
            const uint32_t callback_rva = find_registered_callback(
                data, text, runtime_ranges, xref.instruction_rva, xref.target_rva);
            if (std::find(callbacks.begin(), callbacks.end(), callback_rva) == callbacks.end())
                callbacks.push_back(callback_rva);
        }

        if (callbacks.size() != 1)
        {
            std::ostringstream oss;
            oss << "Expected 1 IsGameRuleEnabled callback, found " << callbacks.size();
            if (!callbacks.empty())
            {
                oss << ": ";
                for (size_t i = 0; i < callbacks.size(); ++i)
                {
                    if (i)
                        oss << ", ";
                    oss << "0x" << std::hex << callbacks[i];
                }
            }
            oss << '.';
            throw PatchError(oss.str());
        }

        add_return_true_job(jobs, data, sections,
                            "Patch #4 (IsGameRuleEnabled via string xref)",
                            callbacks.front());

        return jobs;
    }

    void apply_patch(std::vector<uint8_t> &data, const PatchJob &job)
    {
        std::cout << '\n'
                  << job.label << " found at offset: 0x"
                  << std::hex << job.offset << " (RVA 0x" << job.rva << ')'
                  << std::dec << '\n';

        if (debug_info)
            std::cout << "Applying " << job.label << "...\n\n";

        for (size_t i = 0; i < RETURN_TRUE.size(); ++i)
        {
            const size_t absolute_pos = job.offset + i;
            const uint8_t original = data[absolute_pos];
            const uint8_t replacement = RETURN_TRUE[i];

            if (debug_info)
            {
                std::cout << "0x" << std::hex << std::setfill('0')
                          << std::setw(8) << absolute_pos << ": 0x"
                          << std::setw(2) << static_cast<unsigned>(original)
                          << " -> 0x" << std::setw(2)
                          << static_cast<unsigned>(replacement)
                          << std::dec << '\n';
            }

            data[absolute_pos] = replacement;
        }
    }

    [[nodiscard]] int make_patch(const fs::path &filepath)
    {
        auto data_opt = read_file(filepath);
        if (!data_opt)
        {
            std::cerr << "Error: Failed to read " << filepath << '\n';
            return 1;
        }

        auto &data = *data_opt;

        try
        {
            const auto sections = parse_pe_sections(data);

            // Locate and validate every target before changing any bytes.
            const auto patch_jobs = prepare_patch_jobs(data, sections);

            if (patch_jobs.empty())
            {
                std::cout << "\nSuccess: all patches were already applied previously. "
                             "No changes are needed.\n";
                return 0;
            }

            auto backup_path = filepath;
            backup_path += EU5_BACKUP_SUFFIX;
            if (!create_backup(filepath, backup_path))
            {
                std::cerr << "Error: Failed to create backup.\n";
                return 1;
            }
            std::cout << "Backup created: " << backup_path << '\n';

            for (const auto &job : patch_jobs)
                apply_patch(data, job);

            if (!write_file(filepath, data))
            {
                std::cerr << "Error: Failed to write patched file.\n";
                return 1;
            }

            std::cout << "\nEU5 is successfully patched.\n";
            return 0;
        }
        catch (const PatchError &e)
        {
            std::cerr << "Error: " << e.what() << '\n';
            return 1;
        }
    }

} // namespace

int main()
{
    const auto target_path_opt = locate_eu5();
    if (!target_path_opt)
    {
        std::cerr << "eu5.exe not found.\n"
                  << "Place this executable in .../Europa Universalis V/binaries/\n";
        std::cout << "Press Enter to exit...";
        std::cin.get();
        return 1;
    }

    const auto &target_path = *target_path_opt;
    std::cout << "Path: " << target_path << '\n';

    const int result = make_patch(target_path);

    std::cout << "Press Enter to exit...";
    std::cin.get();
    return result;
}
