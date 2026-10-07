// Fault-injection helpers for storage/IO robustness tests.
#pragma once

#include <hdf5.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace mib::test {

// Replace an image dataset with a group while the writer is drained. Subsequent
// appends fail inside HDF5, but experiment metadata remains writable.
inline bool blockHdf5ImageAppends(const std::string& path) {
    const hid_t access = H5Pcreate(H5P_FILE_ACCESS);
    if (access < 0) return false;
    H5Pset_fclose_degree(access, H5F_CLOSE_STRONG);
#if H5_VERSION_GE(1, 10, 7)
    H5Pset_file_locking(access, 0, 1);
#endif
    const hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDWR, access);
    H5Pclose(access);
    if (file < 0) return false;
    bool ok = true;
    for (const char* name : {"/valid_frames/images", "/invalid_frames/images"}) {
        const std::string backup = std::string(name) + "_fault_backup";
        if (H5Lexists(file, name, H5P_DEFAULT) > 0)
            ok = H5Lmove(file, name, file, backup.c_str(), H5P_DEFAULT, H5P_DEFAULT) >= 0 && ok;
        const hid_t group = H5Gcreate2(file, name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (group < 0)
            ok = false;
        else
            H5Gclose(group);
    }
    H5Fclose(file);
    return ok;
}

inline bool restoreHdf5ImageAppends(const std::string& path) {
    const hid_t access = H5Pcreate(H5P_FILE_ACCESS);
    if (access < 0) return false;
    H5Pset_fclose_degree(access, H5F_CLOSE_STRONG);
#if H5_VERSION_GE(1, 10, 7)
    H5Pset_file_locking(access, 0, 1);
#endif
    const hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDWR, access);
    H5Pclose(access);
    if (file < 0) return false;
    bool ok = true;
    for (const char* name : {"/valid_frames/images", "/invalid_frames/images"}) {
        ok = H5Ldelete(file, name, H5P_DEFAULT) >= 0 && ok;
        const std::string backup = std::string(name) + "_fault_backup";
        if (H5Lexists(file, backup.c_str(), H5P_DEFAULT) > 0)
            ok = H5Lmove(file, backup.c_str(), file, name, H5P_DEFAULT, H5P_DEFAULT) >= 0 && ok;
    }
    H5Fclose(file);
    return ok;
}

// A path whose total length exceeds the Windows MAX_PATH (260) limit.
inline std::filesystem::path longPath(const std::filesystem::path& base,
                                      const std::string& leaf = "experiment.h5")
{
    const std::string seg(80, 'x');
    return base / seg / seg / seg / leaf;
}

// Make a directory read-only (best effort, cross-platform via std::filesystem
// permissions). Returns false if the platform/filesystem ignored it.
inline bool makeReadOnly(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::permissions(
        dir,
        std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
            std::filesystem::perms::others_write,
        std::filesystem::perm_options::remove, ec);
    return !ec;
}

inline void restoreWritable(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::permissions(dir, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, ec);
}

} // namespace mib::test
