/**
 * EU5 Patcher - Enable Achievements Unconditionally
 * Compile: cl /std:c++17 /O2 /EHsc patch.cpp
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "Advapi32.lib")
#endif

namespace fs = std::filesystem;

// Constants

// Achievement Check: CanGetAchievements
constexpr std::string_view PATTERN1 =
    "80 ?? ?? ?? ?? ?? 00 75 ?? "
    "80 ?? ?? ?? ?? ?? 00 74 ?? "
    "80 ?? ?? ?? ?? ?? 00 74 ?? "
    "80 ?? ?? ?? ?? ?? 00";
constexpr std::string_view PATTERN_REPLACE1 = "80 ?? ?? ?? ?? ?? 00 eb";

// Ironman Save and Load: GameIsIronman
constexpr std::string_view PATTERN2 =
    "74 ?? "
    "?? ?? ?? "
    "ff ?? ?? "
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "74 ?? "
    "32 c0";

constexpr std::string_view PATTERN_REPLACE2 =
    "eb ?? "
    "?? ?? ?? "
    "ff ?? ?? "
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "74 ?? "
    "b0 00";

// Ironman Console: BLOCK_COMMAND_MULTIPLAYER_IRONMAN
constexpr std::string_view PATTERN3 =
    "00 "
    "75 ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "32 c0 "
    "48 ?? ?? ?? "
    "c3 "
    "b0 01";

constexpr std::string_view PATTERN_REPLACE3 =
    "00 "
    "75 ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "32 c0 "
    "48 ?? ?? ?? "
    "c3 "
    "b0 00";

// Save: SaveIngame
constexpr std::string_view PATTERN4 =
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "48 ?? ?? "
    "e8";

constexpr std::string_view PATTERN_REPLACE4 =
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 09";

// Load: LoadIngame
constexpr std::string_view PATTERN5 =
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "83 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "48 ?? ?? "
    "e8";

constexpr std::string_view PATTERN_REPLACE5 =
    "e8 ?? ?? ?? ?? 80 ?? ?? ?? ?? ?? 09";

struct PatchDefinition
{
    std::string_view label;
    std::string_view pattern;
    std::string_view replacement;
};

const std::vector<PatchDefinition> PATCH_DEFINITIONS = {
    {"Patch #1", PATTERN1, PATTERN_REPLACE1},
    {"Patch #2", PATTERN2, PATTERN_REPLACE2},
    {"Patch #3", PATTERN3, PATTERN_REPLACE3},
    {"Patch #4", PATTERN4, PATTERN_REPLACE4},
    {"Patch #5", PATTERN5, PATTERN_REPLACE5},
};

constexpr std::string_view EU5_PATH = "eu5.exe";
constexpr std::string_view GAME_FOLDER = "Europa Universalis V";
constexpr std::string_view APP_ID = "3450310";
constexpr std::string_view EU5_BACKUP_SUFFIX = ".backup";

bool debuge_info = false;

/**
 * Represents a single byte in a pattern, which can be either
 * a specific value or a wildcard.
 */
struct PatternByte
{
    uint8_t value{0};
    bool is_wildcard{false};

    static PatternByte wildcard() { return {0, true}; }
    static PatternByte exact(uint8_t v) { return {v, false}; }
};

/**
 * Parse a pattern string (e.g., "80 ?? AB") into a vector of PatternBytes.
 */
[[nodiscard]] std::vector<PatternByte> parse_pattern(std::string_view pattern_str)
{
    std::vector<PatternByte> result;
    std::istringstream iss{std::string(pattern_str)};
    std::string token;

    while (iss >> token)
    {
        if (token == "??")
        {
            result.push_back(PatternByte::wildcard());
        }
        else
        {
            result.push_back(PatternByte::exact(
                static_cast<uint8_t>(std::stoi(token, nullptr, 16))));
        }
    }

    return result;
}

struct PatternSearchResult
{
    std::vector<size_t> offsets;
};

/**
 * Find a pattern in data, returning the offset if found.
 * Supports wildcard bytes in the pattern.
 */
[[nodiscard]] PatternSearchResult find_pattern(
    const std::vector<uint8_t> &data,
    const std::vector<PatternByte> &pattern)
{
    PatternSearchResult result{};

    if (pattern.empty() || data.size() < pattern.size())
    {
        return result;
    }

    const size_t pattern_len = pattern.size();
    const size_t search_end = data.size() - pattern_len;

    for (size_t i = 0; i <= search_end; ++i)
    {
        bool match = true;

        for (size_t j = 0; j < pattern_len && match; ++j)
        {
            if (!pattern[j].is_wildcard && data[i + j] != pattern[j].value)
            {
                match = false;
            }
        }

        if (match)
        {
            result.offsets.push_back(i);
        }
    }

    return result;
}

/**
 * Apply the replacement bytes at the given offset, printing progress details.
 */
void apply_patch_bytes(std::vector<uint8_t> &data,
                       size_t offset,
                       const std::vector<PatternByte> &replacement,
                       std::string_view label)
{
    std::cout << '\n'
              << label << " found at offset: 0x"
              << std::hex << offset << std::dec << '\n';

    if (debuge_info)
        std::cout << "Applying " << label << "...\n\n";

    for (size_t i = 0; i < replacement.size(); ++i)
    {
        const size_t absolute_pos = offset + i;
        const uint8_t original = data[absolute_pos];
        const auto &pb = replacement[i];

        if (debuge_info)
            std::cout << "0x" << std::hex << std::setfill('0')
                      << std::setw(6) << absolute_pos << ": 0x"
                      << std::setw(2) << static_cast<int>(original) << " -> 0x";

        if (pb.is_wildcard)
        {
            if (debuge_info)
                std::cout << std::setw(2) << static_cast<int>(original)
                          << " (unchanged)";
        }
        else
        {
            if (debuge_info)
                std::cout << std::setw(2) << static_cast<int>(pb.value);

            data[absolute_pos] = pb.value;
        }
        if (debuge_info)
            std::cout << std::dec << '\n';
    }
}

/**
 * Read entire file into a byte vector.
 */
[[nodiscard]] std::optional<std::vector<uint8_t>> read_file(const fs::path &filepath)
{
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return std::nullopt;
    }

    const auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(buffer.data()), size))
    {
        return std::nullopt;
    }

    return buffer;
}

/**
 * Write byte vector to file.
 */
[[nodiscard]] bool write_file(const fs::path &filepath, const std::vector<uint8_t> &data)
{
    std::ofstream file(filepath, std::ios::binary);
    if (!file)
    {
        return false;
    }

    file.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
    return file.good();
}

/**
 * Create a backup of the target file.
 */
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
    {
        return std::nullopt;
    }

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

    // Remove possible trailing nulls introduced by the API call
    if (!buffer.empty() && buffer.back() == L'\0')
    {
        buffer.pop_back();
    }

    return buffer;
}
#endif

[[nodiscard]] std::optional<fs::path> get_steam_install_path()
{
#ifdef _WIN32
    auto primary = read_registry_string(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (primary)
    {
        return fs::path(*primary);
    }

    auto fallback = read_registry_string(
        HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (fallback)
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
            {
                return p;
            }
        }
    }
#endif
    return std::nullopt;
}

[[nodiscard]] std::string trim(const std::string &s)
{
    const auto first = s.find_first_not_of(" \t\n\r");
    if (first == std::string::npos)
    {
        return {};
    }
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
        {
            break;
        }

        const auto end = line.find('"', start + 1);
        if (end == std::string::npos)
        {
            break;
        }

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
    {
        return results;
    }

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
            {
                in_apps_block = false;
            }
            continue;
        }

        if (!in_apps_block)
        {
            if (tokens[0] == "path" && tokens.size() >= 2)
            {
                current_path = tokens[1];
                continue;
            }

            // 进入 apps 块
            if (tokens[0] == "apps")
            {
                in_apps_block = true;
            }
            continue;
        }

        // Within "apps" block
        // 检查 AppID
        if (tokens[0] == target_appid)
        {
            results.push_back(fs::path(current_path));
        }
    }

    return results;
}

[[nodiscard]] std::optional<fs::path> get_game_folder(std::string_view name)
{
    const auto steam_path_opt = get_steam_install_path();
    if (!steam_path_opt)
    {
        return std::nullopt;
    }

    const auto library_db = *steam_path_opt / "steamapps" / "libraryfolders.vdf";
    const auto library_paths = find_all_steam_libraries_with_app(library_db, APP_ID);
    for (const auto &library_path : library_paths)
    {
        const auto game_folder = library_path / "steamapps" / "common" / std::string(name);
        const auto binaries_path = game_folder / "binaries" / EU5_PATH;
        if (fs::is_regular_file(binaries_path))
        {
            return game_folder;
        }
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<fs::path> locate_eu5()
{
    const fs::path local_path{EU5_PATH};
    if (fs::exists(local_path))
    {
        return local_path;
    }

    const auto game_folder = get_game_folder(GAME_FOLDER);
    if (game_folder)
    {
        const auto steam_path = *game_folder / "binaries" / EU5_PATH;
        if (fs::exists(steam_path))
        {
            return steam_path;
        }
    }

    return std::nullopt;
}

/**
 * Apply the patch to the target file.
 * Returns: 0 = success, 1 = error
 */
[[nodiscard]] int make_patch(const fs::path &filepath)
{
    // Read file
    auto data_opt = read_file(filepath);
    if (!data_opt)
    {
        std::cerr << "Error: Failed to read " << filepath << '\n';
        return 1;
    }
    auto &data = *data_opt;

    std::vector<size_t> offsets;
    std::vector<std::vector<PatternByte>> replacements;
    std::vector<std::string_view> labels;

    // Parse patterns and determine offsets
    for (const auto &patch_def : PATCH_DEFINITIONS)
    {
        const auto pattern = parse_pattern(patch_def.pattern);
        const auto search_result = find_pattern(data, pattern);

        if (search_result.offsets.empty())
        {
            std::cerr << "Error: " << patch_def.label
                      << " not found. Have you patched it before?\n"
                      << "If not, the pattern may need to be updated.\n";
            return 1;
        }

        const auto replacement = parse_pattern(patch_def.replacement);
        for (size_t offset : search_result.offsets)
        {
            offsets.push_back(offset);
            replacements.push_back(replacement);
            labels.push_back(patch_def.label);
        }
    }

    // Create backup only after confirming all patterns exist
    auto backup_path = filepath;
    backup_path += EU5_BACKUP_SUFFIX;

    if (!create_backup(filepath, backup_path))
    {
        std::cerr << "Error: Failed to create backup.\n";
        return 1;
    }
    std::cout << "Backup created: " << backup_path << '\n';

    for (size_t i = 0; i < offsets.size(); ++i)
    {
        apply_patch_bytes(data, offsets[i], replacements[i], labels[i]);
    }

    // Write back
    if (!write_file(filepath, data))
    {
        std::cerr << "Error: Failed to write patched file.\n";
        return 1;
    }

    std::cout << "\nEU5 is successfully patched.\n";
    return 0;
}

int main()
{
    const auto target_path_opt = locate_eu5();
    if (!target_path_opt)
    {
        std::cerr << "eu5.exe not found.\n"
                  << "Place this file in .../Europa Universalis V/binaries/\n";
        std::cout << "Press Enter to exit...";
        std::cin.get();
        return 1;
    }

    const auto &target_path = *target_path_opt;
    std::cout << "Path: " << target_path << '\n';

    // Apply patch
    const int result = make_patch(target_path);

    std::cout << "Press Enter to exit...";
    std::cin.get();

    return result;
}