// Save data backup in the user's cloud (tvOS: Library/Caches is the only
// writable place and the system may empty it while the app is not running,
// docs/knowledge/tvos-als-zielplattform.md). One record per key holds an
// opaque blob and a sequence number; lgp/boot/save_backup.hpp decides what
// goes in and which side is newer.
//
//   tvOS:      CloudKit, private database of the app's container
//              (platform/apple/src/cloud_save_cloudkit.mm, needs the iCloud
//              entitlement, cmake/LgpIos.cmake CLOUD_SAVES)
//   elsewhere: none (make_cloud_save returns nullptr; the folders persist)
//
// All calls block up to their timeout and may be made from any thread except
// the one running the platform's main loop (CloudKit answers on its own
// queues; SDL's UIKit loop must keep running).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lgp::platform {

enum class CloudStatus {
    Available,    // signed in, iCloud on for the app
    NoAccount,    // no iCloud account on the device
    Restricted,   // parental controls or a profile forbid it
    Unavailable,  // temporarily (network, iCloud service), try later
    Error,        // anything else; see detail
};
const char* cloud_status_name(CloudStatus status);

struct CloudRecord {
    uint64_t seq = 0;  // larger = newer
    std::vector<uint8_t> data;
};

class ICloudSave {
public:
    virtual ~ICloudSave() = default;
    virtual CloudStatus status(int timeout_ms, std::string* detail) = 0;
    // *found = false (and true returned) when there is no record for key.
    virtual bool fetch(const std::string& key, int timeout_ms, bool* found, CloudRecord* out,
                       std::string* error) = 0;
    // Replaces the record for key. A record written elsewhere since this side
    // last saw it, with seq not smaller than record.seq, is kept: false and
    // *cloud_newer = true (retrying will not help).
    virtual bool store(const std::string& key, const CloudRecord& record, int timeout_ms, bool* cloud_newer,
                       std::string* error) = 0;
};

// The platform's cloud backup for `container` (tvOS: "iCloud.<bundle id>"),
// or nullptr where there is none or container is empty.
std::unique_ptr<ICloudSave> make_cloud_save(const std::string& container);

}  // namespace lgp::platform
