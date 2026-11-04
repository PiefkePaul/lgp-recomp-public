// CloudKit backend of cloud_save.hpp (iOS and tvOS, one container). One record of type "SaveBlob"
// per key in the default zone of the private database: field "seq" (int64)
// and field "data" (CKAsset, the blob; a record field holds at most 1 MB and
// the saves are about that). Built with ARC (-fobjc-arc, set per file in
// platform/CMakeLists.txt): the __block results below and the records kept in
// records_ are only retained under ARC.
#include "lgp/platform/cloud_save.hpp"

#if !__has_feature(objc_arc)
#error "cloud_save_cloudkit.mm needs ARC (-fobjc-arc)"
#endif

#import <CloudKit/CloudKit.h>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>

#include <map>
#include <mutex>

namespace lgp::platform {
namespace {

NSString* const kRecordType = @"SaveBlob";

std::string to_string(NSString* s) { return s != nil ? std::string(s.UTF8String) : std::string(); }

std::string error_text(NSError* e) {
    if (e == nil)
        return "timed out";
    return to_string(e.localizedDescription) + " (CKError " + std::to_string(static_cast<long>(e.code)) + ")";
}

// The completion handlers run on CloudKit's queues; the caller waits here.
bool wait_for(dispatch_semaphore_t done, int timeout_ms) {
    return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t(timeout_ms) * int64_t(NSEC_PER_MSEC))) ==
           0;
}

uint64_t record_seq(CKRecord* record) {
    id value = record[@"seq"];
    return [value isKindOfClass:[NSNumber class]] ? static_cast<uint64_t>([(NSNumber*)value unsignedLongLongValue]) : 0;
}

// Each call runs on the backup's worker thread (a std::thread without its own
// pool), so the objects CloudKit hands back autoreleased are drained per call.
class CloudKitSave final : public ICloudSave {
public:
    explicit CloudKitSave(CKContainer* container) : container_(container), db_(container.privateCloudDatabase) {}

    CloudStatus status(int timeout_ms, std::string* detail) override {
        @autoreleasepool {
            __block CKAccountStatus account = CKAccountStatusCouldNotDetermine;
            __block NSError* failure = nil;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [container_ accountStatusWithCompletionHandler:^(CKAccountStatus s, NSError* e) {
              account = s;
              failure = e;
              dispatch_semaphore_signal(done);
            }];
            if (!wait_for(done, timeout_ms)) {
                *detail = "no answer from iCloud";
                return CloudStatus::Unavailable;
            }
            switch (account) {
            case CKAccountStatusAvailable: return CloudStatus::Available;
            case CKAccountStatusNoAccount: *detail = "no iCloud account"; return CloudStatus::NoAccount;
            case CKAccountStatusRestricted: *detail = "iCloud restricted"; return CloudStatus::Restricted;
            case CKAccountStatusTemporarilyUnavailable:
                *detail = "iCloud temporarily unavailable";
                return CloudStatus::Unavailable;
            default: *detail = error_text(failure); return CloudStatus::Error;
            }
        }
    }

    bool fetch(const std::string& key, int timeout_ms, bool* found, CloudRecord* out, std::string* error) override {
        @autoreleasepool {
            __block CKRecord* got = nil;
            __block NSError* failure = nil;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [db_ fetchRecordWithID:record_id(key)
                 completionHandler:^(CKRecord* r, NSError* e) {
                   got = r;
                   failure = e;
                   dispatch_semaphore_signal(done);
                 }];
            if (!wait_for(done, timeout_ms)) {
                *error = error_text(nil);
                return false;
            }
            if (failure != nil && failure.code == CKErrorUnknownItem) {
                *found = false;
                return true;
            }
            if (got == nil) {
                *error = error_text(failure);
                return false;
            }
            id asset = got[@"data"];
            NSData* bytes = [asset isKindOfClass:[CKAsset class]] && [(CKAsset*)asset fileURL] != nil
                                ? [NSData dataWithContentsOfURL:[(CKAsset*)asset fileURL]]
                                : nil;
            if (bytes == nil) {
                *error = "record without data";
                return false;
            }
            out->seq = record_seq(got);
            out->data.assign(static_cast<const uint8_t*>(bytes.bytes), static_cast<const uint8_t*>(bytes.bytes) + bytes.length);
            remember(key, got);
            *found = true;
            return true;
        }
    }

    bool store(const std::string& key, const CloudRecord& record, int timeout_ms, bool* cloud_newer,
               std::string* error) override {
        @autoreleasepool {
            *cloud_newer = false;
            // The asset is read from a file while the operation runs.
            NSString* name = [NSString stringWithFormat:@"lgp-cloud-%@.bin", [NSUUID UUID].UUIDString];
            NSURL* file = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:name]];
            NSData* bytes = [NSData dataWithBytes:record.data.data() length:record.data.size()];
            if (![bytes writeToURL:file atomically:YES]) {
                *error = "cannot write " + to_string(file.path);
                return false;
            }
            // The record last seen carries the change tag; with
            // IfServerRecordUnchanged a record changed elsewhere since is
            // reported, not overwritten.
            // A copy: a save that fails must not leave our numbers in the record
            // remembered as the cloud's.
            CKRecord* target = [known(key) copy];
            if (target == nil)
                target = [[CKRecord alloc] initWithRecordType:kRecordType recordID:record_id(key)];
            bool ok = false;
            for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
                // A record seen here earlier (fetched, or returned by a refused
                // save) carries its change tag, so a save onto it goes through:
                // its number decides before anything is written.
                if (record_seq(target) >= record.seq) {
                    *cloud_newer = true;
                    *error = "a newer save is in iCloud";
                    break;
                }
                target[@"seq"] = @(static_cast<unsigned long long>(record.seq));
                target[@"data"] = [[CKAsset alloc] initWithFileURL:file];
                NSError* failure = nil;
                CKRecord* saved = save(target, timeout_ms, &failure);
                if (saved != nil) {
                    remember(key, saved);
                    ok = true;
                    break;
                }
                CKRecord* server = failure.code == CKErrorServerRecordChanged
                                       ? failure.userInfo[CKRecordChangedErrorServerRecordKey]
                                       : nil;
                if (server == nil) {
                    *error = error_text(failure);
                    break;
                }
                remember(key, server);
                // The same number written elsewhere counts as newer too (another
                // device's state, or this upload, timed out but stored): the
                // check at the top of the loop refuses it.
                target = [server copy];
            }
            [[NSFileManager defaultManager] removeItemAtURL:file error:nil];
            return ok;
        }
    }

private:
    static CKRecordID* record_id(const std::string& key) {
        return [[CKRecordID alloc] initWithRecordName:[NSString stringWithUTF8String:key.c_str()]];
    }

    CKRecord* save(CKRecord* record, int timeout_ms, NSError** failure) {
        __block CKRecord* saved = nil;
        __block NSError* item_error = nil;
        __block NSError* op_error = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        CKModifyRecordsOperation* op = [[CKModifyRecordsOperation alloc] initWithRecordsToSave:@[ record ]
                                                                             recordIDsToDelete:nil];
        op.savePolicy = CKRecordSaveIfServerRecordUnchanged;
        op.qualityOfService = NSQualityOfServiceUserInitiated;
        op.perRecordSaveBlock = ^(CKRecordID*, CKRecord* r, NSError* e) {
          saved = r;
          item_error = e;
        };
        // modifyRecordsResultBlock is Swift only; this one is deprecated for
        // Swift alone (AppleTVOS27.0 SDK).
        op.modifyRecordsCompletionBlock = ^(NSArray<CKRecord*>*, NSArray<CKRecordID*>*, NSError* e) {
          op_error = e;
          dispatch_semaphore_signal(done);
        };
        [db_ addOperation:op];
        if (!wait_for(done, timeout_ms)) {
            [op cancel];
            *failure = nil;
            return nil;
        }
        if (saved != nil && item_error == nil)
            return saved;
        *failure = item_error != nil ? item_error : op_error;
        return nil;
    }

    void remember(const std::string& key, CKRecord* record) {
        std::lock_guard lock(mutex_);
        records_[key] = record;
    }
    CKRecord* known(const std::string& key) {
        std::lock_guard lock(mutex_);
        auto it = records_.find(key);
        return it != records_.end() ? it->second : nil;
    }

    CKContainer* container_;
    CKDatabase* db_;
    std::mutex mutex_;
    std::map<std::string, CKRecord*> records_;  // last record seen per key (its change tag)
};

}  // namespace

std::unique_ptr<ICloudSave> make_cloud_save(const std::string& container) {
    if (container.empty())
        return nullptr;
    @try {
        // Throws when the app is not signed with this container.
        CKContainer* c = [CKContainer containerWithIdentifier:[NSString stringWithUTF8String:container.c_str()]];
        return c != nil ? std::make_unique<CloudKitSave>(c) : nullptr;
    } @catch (NSException* e) {
        return nullptr;
    }
}

}  // namespace lgp::platform
