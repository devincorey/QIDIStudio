#ifndef slic3r_ResultFilePath_hpp_
#define slic3r_ResultFilePath_hpp_

#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

inline bool path_is_same_or_child(boost::filesystem::path candidate,
                                  boost::filesystem::path parent)
{
    candidate = candidate.lexically_normal();
    parent = parent.lexically_normal();
    if (parent.empty())
        return false;

    auto candidate_it = candidate.begin();
    for (auto parent_it = parent.begin(); parent_it != parent.end(); ++parent_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *candidate_it != *parent_it)
            return false;
    }
    return true;
}

inline boost::filesystem::path resolve_result_file_path(
    const std::string &output_directory,
    const boost::filesystem::path &current_directory,
    const boost::filesystem::path &resources_directory,
    const boost::filesystem::path &fallback_directory)
{
    if (!output_directory.empty())
        return boost::filesystem::path(output_directory) / "result.json";

    if (!fallback_directory.empty() &&
        path_is_same_or_child(current_directory, resources_directory))
        return fallback_directory / "result.json";

    return current_directory / "result.json";
}

} // namespace Slic3r

#endif
