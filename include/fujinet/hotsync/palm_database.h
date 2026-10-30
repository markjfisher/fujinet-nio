#pragma once

// In-memory form of a Palm .pdb (record) or .prc (resource) file, converted
// to and from the shapes the DLP commands take.
// Reference: Palm File Format Specification (DatabaseHdrType)

#include "fujinet/hotsync/dlp_client.h"

#include <cstdint>
#include <vector>

namespace fujinet::hotsync {

struct PalmDatabase {
    DlpDbInfo info;
    std::uint32_t created{0}; // seconds since 1904-01-01
    std::uint32_t modified{0};
    std::uint32_t backedUp{0};
    Bytes appInfo;
    Bytes sortInfo;
    std::vector<DlpRecord> records;     // when info.flags lacks kDbResource
    std::vector<DlpResource> resources; // when info.flags has kDbResource

    bool is_resource_db() const { return (info.flags & kDbResource) != 0; }

    bool parse(const Bytes& file);
    Bytes serialize() const;
};

// Palm timestamps count seconds from 1904-01-01 with no time zone.
std::uint32_t palm_seconds_from_date(const DlpDateTime& t);
DlpDateTime palm_date_from_seconds(std::uint32_t seconds);

} // namespace fujinet::hotsync
