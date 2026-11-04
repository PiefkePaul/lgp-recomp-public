// Cloud save backup (cloud_save.hpp): the parts without a cloud.
#include "lgp/platform/cloud_save.hpp"

namespace lgp::platform {

const char* cloud_status_name(CloudStatus status) {
    switch (status) {
    case CloudStatus::Available: return "available";
    case CloudStatus::NoAccount: return "no iCloud account";
    case CloudStatus::Restricted: return "restricted";
    case CloudStatus::Unavailable: return "temporarily unavailable";
    case CloudStatus::Error: return "error";
    }
    return "?";
}

#if !defined(LGP_PLATFORM_CLOUDKIT)
std::unique_ptr<ICloudSave> make_cloud_save(const std::string&) { return nullptr; }
#endif

}  // namespace lgp::platform
