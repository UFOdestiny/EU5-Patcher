#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

const std::string PATTERN = "80 ?? ?? ?? ?? ?? 00 75 ?? 80 ?? ?? ?? ?? ?? 00 74 ?? 80 ?? ?? ?? ?? ?? 00 74 ?? 80 ?? ?? ?? ?? ?? 00";
const std::string PATTERN_REPLACE = "80 ?? ?? ?? ?? ?? 00 eb";
const std::string EU5_PATH = "eu5.exe";
const std::string EU5_BACKUP_PATH = "eu5.exe.backup";

struct PatternByte
{
    uint8_t value;
    bool is_wildcard;
};

// Parse pattern string into efficient structure
std::vector<PatternByte> parse_pattern(const std::string &pattern_str)
{
    std::vector<PatternByte> result;
    std::istringstream iss(pattern_str);
    std::string token;

    while (iss >> token)
    {
        PatternByte pb;
        if (token == "??")
        {
            pb.is_wildcard = true;
            pb.value = 0;
        }
        else
        {
            pb.is_wildcard = false;
            pb.value = static_cast<uint8_t>(std::stoi(token, nullptr, 16));
        }
        result.push_back(pb);
    }

    return result;
}

// Boyer-Moore-Horspool pattern matching for better performance
size_t find_pattern(const std::vector<uint8_t> &data, const std::vector<PatternByte> &pattern)
{
    if (pattern.empty() || data.size() < pattern.size())
    {
        return std::string::npos;
    }

    const size_t pattern_len = pattern.size();
    const size_t data_len = data.size();

    // Simple pattern matching with wildcard support
    for (size_t i = 0; i <= data_len - pattern_len; ++i)
    {
        bool match = true;

        for (size_t j = 0; j < pattern_len; ++j)
        {
            if (!pattern[j].is_wildcard && data[i + j] != pattern[j].value)
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            return i;
        }
    }

    return std::string::npos;
}

// Fast file reading using reserve
std::vector<uint8_t> read_file(const std::string &filename)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer;
    buffer.reserve(size);
    buffer.assign(std::istreambuf_iterator<char>(file),
                  std::istreambuf_iterator<char>());

    return buffer;
}

// Fast file writing
void write_file(const std::string &filename, const std::vector<uint8_t> &data)
{
    std::ofstream file(filename, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Cannot write file: " + filename);
    }

    file.write(reinterpret_cast<const char *>(data.data()), data.size());
}

void make_patch(const std::string &filename)
{
    // Make Backup
    try
    {
        fs::copy_file(EU5_PATH, EU5_BACKUP_PATH, fs::copy_options::overwrite_existing);
        std::cout << "Backup is made: " << EU5_BACKUP_PATH << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to create backup: " << e.what() << std::endl;
        return;
    }

    // Read file
    std::vector<uint8_t> data;
    try
    {
        data = read_file(filename);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to read file: " << e.what() << std::endl;
        return;
    }

    // Parse patterns
    auto pattern = parse_pattern(PATTERN);
    auto replace_pattern = parse_pattern(PATTERN_REPLACE);

    // Find pattern
    size_t index = find_pattern(data, pattern);

    if (index == std::string::npos)
    {
        std::cout << "Fail: The pattern is not founded. Have you patched it before? "
                  << "If no, the pattern needs to update." << std::endl;
        return;
    }

    // Replace bytes
    for (size_t i = 0; i < replace_pattern.size(); ++i)
    {
        if (replace_pattern[i].is_wildcard)
        {
            std::cout << index + i << ": 0x" << std::hex
                      << static_cast<int>(data[index + i])
                      << " \t-> 0x" << static_cast<int>(data[index + i])
                      << std::dec << std::endl;
        }
        else
        {
            std::cout << index + i << ": 0x" << std::hex
                      << static_cast<int>(data[index + i])
                      << " \t-> 0x" << static_cast<int>(replace_pattern[i].value)
                      << std::dec << std::endl;
            data[index + i] = replace_pattern[i].value;
        }
    }

    // Write back
    try
    {
        write_file(filename, data);
        std::cout << "EU5 is successfully patched." << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to write file: " << e.what() << std::endl;
    }
}

int main()
{
    // Check Path
    if (!fs::exists(EU5_PATH))
    {
        std::cout << "eu5.exe is not found. "
                  << "Put this file in .../Europa Universalis IV/binaries/" << std::endl;
        std::cout << "Press any key to exit..." << std::endl;
        std::cin.get();
        return 0;
    }

    // Patch
    make_patch(EU5_PATH);

    std::cout << "Press any key to exit..." << std::endl;
    std::cin.get();

    return 0;
}