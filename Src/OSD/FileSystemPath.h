/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2003-2022 The Supermodel Team
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
 * FileSystemPaths.h
 *
 * Header file for OS-dependent Supermodel files locations.
 */

#ifndef INCLUDED_FILESYSTEMPATH_H
#define INCLUDED_FILESYSTEMPATH_H

#include <string>
#include <vector>

namespace Util
{
    namespace Config
    {
        class Node;
    }
}

namespace FileSystemPath
{
    enum PathType { Analysis, Config, Log, NVRAM, Saves, Screenshots, Assets, ROMs }; // Filesystem path types
    constexpr size_t NUM_PATH_TYPES = ROMs + 1;

    bool PathExists(std::string fileSystemPath); // Checks if a directory exists (returns true if exists, false if it doesn't)
    int MakeDir(std::string dir); // Create a directory
    bool MakeDirs(const std::string &path);
    std::string GetPath(PathType pathType, bool createDirectories = true);  // Generates a path to be used by Supermodel files

    /*
     * User-configurable locations, in order of priority: the command line
     * (-nvram-path=<dir>), the environment (SUPERMODEL_NVRAM_PATH) and
     * Supermodel.ini (NVRAMPath). Config is the exception: it is where
     * Supermodel.ini is found, so a setting inside that file cannot relocate
     * it. Both lists are empty where GetPath() ignores these settings, so an
     * option that would do nothing never reaches the .ini or the GUI.
     */
    const std::vector<PathType> &OverridablePathTypes();
    const std::vector<PathType> &ConfigurablePathTypes();

    const char *ConfigKey(PathType pathType);
    const char *CommandLineOption(PathType pathType);
    const char *EnvironmentVariable(PathType pathType);
    const char *Description(PathType pathType);

    void SetOverride(PathType pathType, const std::string &path);
    void SetConfigOverride(PathType pathType, const std::string &path);
    std::string GetOverride(PathType pathType);

    void ApplyEnvironmentOverrides();
    bool ApplyCommandLineOption(const std::string &arg); // Consumes "-<name>-path=<dir>", false if the argument is not one
    void ApplyConfigOverrides(const Util::Config::Node &config);
}


#endif // INCLUDED_FILESYSTEMPATH_H
