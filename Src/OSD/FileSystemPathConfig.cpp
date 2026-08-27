/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2003-2026 The Supermodel Team
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/

/*
 * FileSystemPathConfig.cpp
 *
 * Platform-independent half of FileSystemPath. The per-platform GetPath()
 * implementations consult GetOverride() before falling back to their defaults.
 */

#include "OSD/FileSystemPath.h"
#include "OSD/Logger.h"
#include "Util/NewConfig.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace FileSystemPath
{
    namespace
    {
        struct Descriptor
        {
            const char *configKey;
            const char *commandLine;
            const char *environment;
            const char *description;
        };

        // Indexed by PathType, so the order here must match the order of the enum
        const std::array<Descriptor, NUM_PATH_TYPES> s_descriptors = { {
            { "AnalysisPath",    "-analysis-path",    "SUPERMODEL_ANALYSIS_PATH",    "Graphics analysis files"              },
            { "ConfigPath",      "-config-path",      "SUPERMODEL_CONFIG_PATH",      "Supermodel.ini, Games.xml, Music.xml" },
            { "LogPath",         "-log-path",         "SUPERMODEL_LOG_PATH",         "Supermodel.log"                       },
            { "NVRAMPath",       "-nvram-path",       "SUPERMODEL_NVRAM_PATH",       "Per-game NVRAM"                       },
            { "SavesPath",       "-saves-path",       "SUPERMODEL_SAVES_PATH",       "Save states"                          },
            { "ScreenshotsPath", "-screenshots-path", "SUPERMODEL_SCREENSHOTS_PATH", "Screenshots"                          },
            { "AssetsPath",      "-assets-path",      "SUPERMODEL_ASSETS_PATH",      "Crosshair bitmaps and other assets"   },
            { "ROMPath",         "-rom-path",         "SUPERMODEL_ROM_PATH",         "ROM sets listed in the GUI"           }
        } };

        std::array<std::string, NUM_PATH_TYPES> s_priorityOverrides; // command line and environment
        std::array<std::string, NUM_PATH_TYPES> s_configOverrides;   // Supermodel.ini and the GUI

        // Trailing separators are dropped so GetPath() can append exactly one
        std::string Normalise(const std::string &path)
        {
            static const char *whiteSpace = " \t\r\n";

            size_t first = path.find_first_not_of(whiteSpace);
            if (first == std::string::npos)
            {
                return std::string();
            }
            size_t last = path.find_last_not_of(whiteSpace);
            std::string result = path.substr(first, last - first + 1);

            if (result == "~" || result.compare(0, 2, "~/") == 0)
            {
                const char *home = getenv("HOME");
                if (home && *home)
                {
                    result = std::string(home) + result.substr(1);
                }
            }

            while (result.length() > 1 && (result.back() == '/' || result.back() == '\\'))
            {
                result.pop_back();
            }

            return result;
        }

        // Only the Unix implementation of GetPath() honours these settings, and a
        // setting that is quietly ignored is worse than no setting at all
        constexpr bool SupportsOverrides()
        {
#if defined(SUPERMODEL_WIN32) || defined(SUPERMODEL_OSX)
            return false;
#else
            return true;
#endif
        }

        const std::vector<PathType> s_noPathTypes;
    }

    const std::vector<PathType> &OverridablePathTypes()
    {
        static const std::vector<PathType> pathTypes { Config, ROMs, NVRAM, Saves, Screenshots, Assets, Log, Analysis };
        return SupportsOverrides() ? pathTypes : s_noPathTypes;
    }

    const std::vector<PathType> &ConfigurablePathTypes()
    {
        // Config is missing on purpose: see the header
        static const std::vector<PathType> pathTypes { ROMs, NVRAM, Saves, Screenshots, Assets, Log, Analysis };
        return SupportsOverrides() ? pathTypes : s_noPathTypes;
    }

    const char *ConfigKey(PathType pathType)
    {
        return s_descriptors[pathType].configKey;
    }

    const char *CommandLineOption(PathType pathType)
    {
        return s_descriptors[pathType].commandLine;
    }

    const char *EnvironmentVariable(PathType pathType)
    {
        return s_descriptors[pathType].environment;
    }

    const char *Description(PathType pathType)
    {
        return s_descriptors[pathType].description;
    }

    void SetOverride(PathType pathType, const std::string &path)
    {
        s_priorityOverrides[pathType] = Normalise(path);
    }

    void SetConfigOverride(PathType pathType, const std::string &path)
    {
        s_configOverrides[pathType] = Normalise(path);
    }

    std::string GetOverride(PathType pathType)
    {
        if (!SupportsOverrides())
        {
            return std::string();
        }

        const std::string &priority = s_priorityOverrides[pathType];
        return priority.empty() ? s_configOverrides[pathType] : priority;
    }

    void ApplyEnvironmentOverrides()
    {
        for (PathType pathType : OverridablePathTypes())
        {
            const char *value = getenv(EnvironmentVariable(pathType));
            if (value && *value)
            {
                SetOverride(pathType, value);
            }
        }
    }

    bool ApplyCommandLineOption(const std::string &arg)
    {
        size_t idxEquals = arg.find_first_of('=');
        if (idxEquals == std::string::npos)
        {
            return false;
        }

        std::string option(arg, 0, idxEquals);
        std::string value(arg, idxEquals + 1);

        for (PathType pathType : OverridablePathTypes())
        {
            if (option == CommandLineOption(pathType))
            {
                SetOverride(pathType, value);
                return true;
            }
        }

        return false;
    }

    void ApplyConfigOverrides(const Util::Config::Node &config)
    {
        for (PathType pathType : ConfigurablePathTypes())
        {
            const Util::Config::Node *node = config.TryGet(ConfigKey(pathType));
            SetConfigOverride(pathType, (node && node->Exists()) ? node->ValueAs<std::string>() : std::string());
        }

        // A ConfigPath stored in the configuration file would be self-referential:
        // by the time it can be read, Supermodel.ini has already been located
        static bool warnedAboutConfigPath = false;
        if (SupportsOverrides() && !warnedAboutConfigPath)
        {
            const Util::Config::Node *node = config.TryGet(ConfigKey(Config));
            if (node && node->Exists() && !node->ValueAs<std::string>().empty())
            {
                warnedAboutConfigPath = true;
                ErrorLog("%s in the configuration file is ignored. Use %s=<dir> or the %s environment variable instead.",
                         ConfigKey(Config), CommandLineOption(Config), EnvironmentVariable(Config));
            }
        }
    }

    bool MakeDirs(const std::string &path)
    {
        if (path.empty())
        {
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path), ec);
        return !ec;
    }
}
