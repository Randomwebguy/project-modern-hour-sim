// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Common/AsciiString.h"
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

// The log path is read once. Logging stays off unless GENZH_MODEL_LOG names a file.
inline const std::string &ModelLoadLogPath()
{
    static const std::string path = [] {
        const char *value = std::getenv("GENZH_MODEL_LOG");
        return std::string(value && *value ? value : "");
    }();
    return path;
}

inline bool ModelLoadLogEnabled()
{
    return !ModelLoadLogPath().empty();
}

// Remaster M1 (15 Nov 2026): GENZH_FILE_LOG=<file> lists every file the game opens through TheFileSystem, once per
// (file, source), with the source being "loose" or the archive it came from. Used to find which original-game files
// the headless sim really needs.
inline void RecordFileOpen(const char *filename, const char *source)
{
    static const std::string path = [] {
        const char *value = std::getenv("GENZH_FILE_LOG");
        return std::string(value && *value ? value : "");
    }();
    if (path.empty() || filename == nullptr) return;
    static std::set<std::string> logged;
    static std::mutex mutex;
    static std::ofstream log(path, std::ios::app);
    std::lock_guard<std::mutex> lock(mutex);
    AsciiString name = filename;
    name.toLower();
    const std::string key = std::string(name.str()) + "\t" + (source ? source : "?");
    if (logged.insert(key).second && log) {
        log << key << '\n';
        log.flush();
    }
}

inline void RecordModelLoad(const char *filename, const char *source, unsigned size)
{
    if (!ModelLoadLogEnabled()) return;
    AsciiString model = filename;
    model.toLower();
    if (!model.endsWith(".w3d")) return;
    // A model may have several passes or many instances, but log its source once.
    // perf114: the stream stays open for the process. Opening and closing the file for each new model
    // cost about 5 percent of the main thread on the fortress save; each line is still flushed when written.
    static std::set<std::string> logged;
    static std::mutex mutex;
    static std::ofstream log(ModelLoadLogPath(), std::ios::app);
    std::lock_guard<std::mutex> lock(mutex);
    const std::string key = std::string(model.str()) + "\t" + source;
    if (logged.insert(key).second && log) {
        log << key << '\t' << size << '\n';
        log.flush();
    }
}
