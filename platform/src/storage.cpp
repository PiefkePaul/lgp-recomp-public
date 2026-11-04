#include "lgp/platform/storage.hpp"

namespace lgp::platform {

const char* storage_error_name(StorageError e) {
    switch (e) {
    case StorageError::Ok: return "Ok";
    case StorageError::NotFound: return "NotFound";
    case StorageError::AlreadyExists: return "AlreadyExists";
    case StorageError::NotADirectory: return "NotADirectory";
    case StorageError::IsADirectory: return "IsADirectory";
    case StorageError::NotEmpty: return "NotEmpty";
    case StorageError::ReadOnly: return "ReadOnly";
    case StorageError::InvalidPath: return "InvalidPath";
    case StorageError::OutOfRange: return "OutOfRange";
    case StorageError::Io: return "Io";
    }
    return "?";
}

std::optional<std::string> normalize_path(std::string_view path) {
    if (path.empty())
        return std::string("/");
    if (path.front() != '/')
        return std::nullopt;
    std::string out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/')
            ++i;
        const size_t start = i;
        while (i < path.size() && path[i] != '/')
            ++i;
        const std::string_view part = path.substr(start, i - start);
        if (part.empty())
            break;
        if (part == "." || part == "..")
            return std::nullopt;
        for (char c : part) {
            if (c == '\\' || c == ':' || c == '\0')
                return std::nullopt;
        }
        out += '/';
        out += part;
    }
    if (out.empty())
        out = "/";
    return out;
}

}  // namespace lgp::platform
