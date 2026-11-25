#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib")
#endif
#endif

namespace fs = std::filesystem;

std::string normalize_input_path(std::string raw) {
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) {
        raw.pop_back();
    }
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) {
        raw.erase(raw.begin());
    }

    if (raw.size() >= 2 &&
        ((raw.front() == '"' && raw.back() == '"') || (raw.front() == '\'' && raw.back() == '\''))) {
        raw = raw.substr(1, raw.size() - 2);
    }

    std::string unescaped;
    unescaped.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size() && (raw[i + 1] == ' ' || raw[i + 1] == '\\')) {
            unescaped.push_back(raw[i + 1]);
            ++i;
        } else {
            unescaped.push_back(raw[i]);
        }
    }
    raw = std::move(unescaped);

    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) {
        raw.pop_back();
    }

#if !defined(_WIN32)
    if (!raw.empty() && raw[0] == '~' && (raw.size() == 1 || raw[1] == '/')) {
        const char* home = std::getenv("HOME");
        if (home) {
            raw = std::string(home) + raw.substr(1);
        }
    }
#endif

    return raw;
}

std::string read_file_content(const fs::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool move_to_trash(const fs::path& file_path) {
#if defined(_WIN32)
    std::error_code ec;
    fs::path abs_path = fs::weakly_canonical(file_path, ec);
    if (ec) {
        abs_path = fs::absolute(file_path, ec);
    }

    std::wstring wide_from = abs_path.wstring();
    wide_from.push_back(L'\0');
    wide_from.push_back(L'\0');

    SHFILEOPSTRUCTW file_op{};
    file_op.hwnd = nullptr;
    file_op.wFunc = FO_DELETE;
    file_op.pFrom = wide_from.c_str();
    file_op.pTo = nullptr;
    file_op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    file_op.fAnyOperationsAborted = FALSE;
    file_op.hNameMappings = nullptr;
    file_op.lpszProgressTitle = nullptr;

    int result = SHFileOperationW(&file_op);
    return (result == 0) && !file_op.fAnyOperationsAborted;
#else
    const char* home = std::getenv("HOME");
    if (!home) {
        return false;
    }

    fs::path trash_dir = fs::path(home) / ".Trash";
    std::error_code ec;
    fs::create_directories(trash_dir, ec);
    if (ec) {
        return false;
    }

    std::string stem = file_path.stem().string();
    std::string ext = file_path.extension().string();
    if (stem.empty()) {
        stem = file_path.filename().string();
        ext.clear();
    }

    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dest = trash_dir / (stem + "_" + std::to_string(stamp) + ext);

    int counter = 1;
    while (fs::exists(dest, ec)) {
        dest = trash_dir / (stem + "_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ext);
    }

    fs::rename(file_path, dest, ec);
    return !ec;
#endif
}

int main(int argc, char* argv[]) {
    std::string raw_path;

    if (argc >= 2) {
        raw_path = argv[1];
    } else {
        std::cout << "Enter folder path to scan: ";
        if (!std::getline(std::cin >> std::ws, raw_path)) {
            std::cout << "Error: Failed to read folder path.\n";
            return 1;
        }
    }

    raw_path = normalize_input_path(raw_path);
    if (raw_path.empty()) {
        std::cout << "Error: Folder path cannot be empty.\n";
        return 1;
    }

    fs::path target_path = raw_path;
    std::error_code ec;
    bool exists = fs::exists(target_path, ec);
    bool is_dir = fs::is_directory(target_path, ec);

    std::cout << "Normalized path: [" << target_path.string() << "]\n";
    if (ec) {
        std::cout << "Filesystem detail: " << ec.message() << "\n";
    }

    if (!exists || !is_dir) {
        std::cout << "Error: Folder does not exist.\n";
        return 1;
    }

    std::map<std::string, std::vector<std::string>> files_by_content;

    for (const auto& entry : fs::recursive_directory_iterator(target_path)) {
        if (entry.is_regular_file()) {
            std::string name = entry.path().filename().string();
            if (name == ".DS_Store") {
                continue;
            }
            std::string content = read_file_content(entry.path());
            files_by_content[content].push_back(entry.path().string());
        }
    }

    int deleted_count = 0;
    int skipped_count = 0;

    for (const auto& pair : files_by_content) {
        if (pair.second.size() > 1) {
            std::cout << "\nDuplicate group (" << pair.second.size() << " files):\n";
            std::cout << "  [KEEP] " << pair.second[0] << "\n";

            bool delete_all_in_group = false;

            for (size_t i = 1; i < pair.second.size(); ++i) {
                char choice = 'n';

                if (!delete_all_in_group) {
                    std::cout << "  Delete duplicate: " << pair.second[i] << " ? (y/n/a - all in group): ";
                    std::cin >> choice;

                    if (choice == 'a' || choice == 'A') {
                        delete_all_in_group = true;
                    }
                }

                if (delete_all_in_group || choice == 'y' || choice == 'Y') {
                    if (move_to_trash(pair.second[i])) {
                        std::cout << "    -> Moved to Trash: " << pair.second[i] << "\n";
                        ++deleted_count;
                    } else {
                        std::cout << "    -> Could not move to Trash: " << pair.second[i] << "\n";
                        ++skipped_count;
                    }
                } else {
                    std::cout << "    -> Skipped: " << pair.second[i] << "\n";
                    ++skipped_count;
                }
            }
        }
    }

    if (deleted_count == 0 && skipped_count == 0) {
        std::cout << "\nNo duplicate files found.\n";
    }

    std::cout << "\n--- Summary ---\n";
    std::cout << "Deleted: " << deleted_count << "\n";
    std::cout << "Skipped: " << skipped_count << "\n";

    return 0;
}