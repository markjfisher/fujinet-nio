// Derived from palm-sync (https://github.com/jichu4n/palm-sync),
// Copyright Chuan Ji, Apache License 2.0. See docs/hotsync.md.

// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/dlp_client.h"

#include "fujinet/core/logging.h"

#include <cstring>

namespace fujinet::hotsync {

namespace {

constexpr const char* TAG = "hotsync";

// Some operations (deleting a large database, installing a big resource) keep
// the device busy for a long while before it answers.
constexpr std::uint32_t kReplyWaitMs = 30000;
constexpr std::uint16_t kReadWhole = 0xFFFF;
constexpr std::uint16_t kHostDlpMajor = 1;
constexpr std::uint16_t kHostDlpMinor = 4;

DlpDateTime read_date_time(DlpArgReader& reader)
{
    DlpDateTime t;
    t.year = reader.u16();
    t.month = reader.u8();
    t.day = reader.u8();
    t.hour = reader.u8();
    t.minute = reader.u8();
    t.second = reader.u8();
    reader.skip(1);
    return t;
}

void write_date_time(DlpArgWriter& writer, const DlpDateTime& t)
{
    writer.u16(t.year).u8(t.month).u8(t.day).u8(t.hour).u8(t.minute).u8(t.second).u8(0);
}

void read_type_id(DlpArgReader& reader, char out[5])
{
    const Bytes id = reader.bytes(4);
    std::memcpy(out, id.data(), 4);
    out[4] = '\0';
}

DlpError parse_failure(const DlpArgReader& reader)
{
    return reader.failed() ? DlpError::Transport : DlpError::None;
}

DlpError read_record_reply(const DlpResponse& response, DlpRecord& out)
{
    DlpArgReader reader(response.arg(0));
    out.id = reader.u32();
    reader.skip(4); // index, size
    out.attributes = reader.u8();
    out.category = reader.u8();
    out.data = reader.rest();
    return parse_failure(reader);
}

} // namespace

DlpError DlpClient::execute(const DlpRequest& request, DlpResponse& response)
{
    response = DlpResponse();
    if (!_transport.send(request.encode())) {
        return DlpError::Transport;
    }
    Bytes reply;
    if (!_transport.receive(reply, kReplyWaitMs)) {
        return DlpError::Transport;
    }
    if (!response.decode(request.func, reply)) {
        FN_LOGW(TAG, "malformed reply to DLP 0x%02x", static_cast<unsigned>(request.func));
        return DlpError::Transport;
    }
    return response.error;
}

DlpError DlpClient::read_sys_info(DlpSysInfo& out)
{
    DlpRequest request(DlpFunc::ReadSysInfo);
    request.arg(DlpArgWriter().u16(kHostDlpMajor).u16(kHostDlpMinor));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err != DlpError::None) {
        return err;
    }

    DlpArgReader reader(response.arg(0));
    out.romVersion = reader.u32();
    out.localizationId = reader.u32();
    // Only devices speaking DLP 1.2 or later return the second argument.
    DlpArgReader extended(response.arg(1));
    if (extended.remaining() >= 12) {
        out.dlpMajor = extended.u16();
        out.dlpMinor = extended.u16();
        extended.skip(4);
        out.maxRecordSize = extended.u32();
    }
    return parse_failure(reader);
}

DlpError DlpClient::read_user_info(DlpUserInfo& out)
{
    DlpResponse response;
    const DlpError err = execute(DlpRequest(DlpFunc::ReadUserInfo), response);
    if (err != DlpError::None) {
        return err;
    }

    DlpArgReader reader(response.arg(0));
    out.userId = reader.u32();
    out.viewerId = reader.u32();
    out.lastSyncPc = reader.u32();
    out.successfulSync = read_date_time(reader);
    out.lastSync = read_date_time(reader);
    const std::uint8_t nameLength = reader.u8();
    reader.skip(1); // password length
    out.userName = nameLength > 0 ? reader.cstring() : std::string();
    return parse_failure(reader);
}

DlpError DlpClient::write_user_info(const DlpUserInfo& info, std::uint8_t modFlags)
{
    DlpArgWriter writer;
    writer.u32(info.userId).u32(info.viewerId).u32(info.lastSyncPc);
    write_date_time(writer, info.lastSync);
    writer.u8(modFlags).u8(static_cast<std::uint8_t>(info.userName.size() + 1)).cstring(info.userName);

    DlpRequest request(DlpFunc::WriteUserInfo);
    request.arg(writer);
    DlpResponse response;
    return execute(request, response);
}

DlpError DlpClient::read_db_list(std::uint8_t flags, std::uint16_t start, std::vector<DlpDbInfo>& out,
                                 std::uint16_t& nextIndex)
{
    DlpRequest request(DlpFunc::ReadDBList);
    request.arg(DlpArgWriter().u8(flags).u8(0).u16(start));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err != DlpError::None) {
        return err;
    }

    DlpArgReader reader(response.arg(0));
    const std::uint16_t lastIndex = reader.u16();
    reader.skip(1); // response flags
    const std::uint8_t count = reader.u8();
    for (std::uint8_t i = 0; i < count && !reader.failed(); ++i) {
        const std::uint8_t entrySize = reader.u8();
        const Bytes entry = reader.bytes(entrySize > 0 ? entrySize - 1u : 0u);
        DlpArgReader fields(entry);
        DlpDbInfo info;
        info.miscFlags = fields.u8();
        info.flags = fields.u16();
        read_type_id(fields, info.type);
        read_type_id(fields, info.creator);
        info.version = fields.u16();
        info.modNum = fields.u32();
        info.created = read_date_time(fields);
        info.modified = read_date_time(fields);
        info.backedUp = read_date_time(fields);
        info.index = fields.u16();
        info.name = fields.cstring();
        out.push_back(info);
    }
    nextIndex = static_cast<std::uint16_t>(lastIndex + 1);
    return parse_failure(reader);
}

DlpError DlpClient::open_conduit()
{
    DlpResponse response;
    return execute(DlpRequest(DlpFunc::OpenConduit), response);
}

DlpError DlpClient::open_db(const std::string& name, std::uint8_t mode, std::uint8_t& handle)
{
    DlpRequest request(DlpFunc::OpenDB);
    request.arg(DlpArgWriter().u8(0).u8(mode).cstring(name));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err == DlpError::None) {
        handle = DlpArgReader(response.arg(0)).u8();
    }
    return err;
}

DlpError DlpClient::create_db(const DlpDbInfo& info, std::uint8_t& handle)
{
    DlpRequest request(DlpFunc::CreateDB);
    request.arg(DlpArgWriter()
                    .type_id(info.creator)
                    .type_id(info.type)
                    .u8(0)
                    .u8(0)
                    .u16(info.flags)
                    .u16(info.version)
                    .cstring(info.name));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err == DlpError::None) {
        handle = DlpArgReader(response.arg(0)).u8();
    }
    return err;
}

DlpError DlpClient::close_db(std::uint8_t handle)
{
    DlpRequest request(DlpFunc::CloseDB);
    request.arg(DlpArgWriter().u8(handle));
    DlpResponse response;
    return execute(request, response);
}

DlpError DlpClient::delete_db(const std::string& name)
{
    DlpRequest request(DlpFunc::DeleteDB);
    request.arg(DlpArgWriter().u8(0).u8(0).cstring(name));
    DlpResponse response;
    return execute(request, response);
}

DlpError DlpClient::read_open_db_record_count(std::uint8_t handle, std::uint16_t& count)
{
    DlpRequest request(DlpFunc::ReadOpenDBInfo);
    request.arg(DlpArgWriter().u8(handle));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err == DlpError::None) {
        count = DlpArgReader(response.arg(0)).u16();
    }
    return err;
}

DlpError DlpClient::read_block(DlpFunc func, std::uint8_t handle, Bytes& out)
{
    DlpRequest request(func);
    request.arg(DlpArgWriter().u8(handle).u8(0).u16(0).u16(kReadWhole));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err != DlpError::None) {
        return err;
    }
    DlpArgReader reader(response.arg(0));
    reader.skip(2); // block size
    out = reader.rest();
    return DlpError::None;
}

DlpError DlpClient::write_block(DlpFunc func, std::uint8_t handle, const Bytes& data)
{
    DlpRequest request(func);
    request.arg(DlpArgWriter().u8(handle).u8(0).u16(static_cast<std::uint16_t>(data.size())).bytes(data));
    DlpResponse response;
    return execute(request, response);
}

DlpError DlpClient::read_app_block(std::uint8_t handle, Bytes& out)
{
    return read_block(DlpFunc::ReadAppBlock, handle, out);
}

DlpError DlpClient::read_sort_block(std::uint8_t handle, Bytes& out)
{
    return read_block(DlpFunc::ReadSortBlock, handle, out);
}

DlpError DlpClient::write_app_block(std::uint8_t handle, const Bytes& data)
{
    return write_block(DlpFunc::WriteAppBlock, handle, data);
}

DlpError DlpClient::write_sort_block(std::uint8_t handle, const Bytes& data)
{
    return write_block(DlpFunc::WriteSortBlock, handle, data);
}

DlpError DlpClient::get_sys_date_time(DlpDateTime& out)
{
    DlpResponse response;
    const DlpError err = execute(DlpRequest(DlpFunc::GetSysDateTime), response);
    if (err != DlpError::None) {
        return err;
    }
    DlpArgReader reader(response.arg(0));
    out = read_date_time(reader);
    return parse_failure(reader);
}

DlpError DlpClient::read_record_by_index(std::uint8_t handle, std::uint16_t index, DlpRecord& out)
{
    DlpRequest request(DlpFunc::ReadRecord);
    request.arg(DlpArgWriter().u8(handle).u8(0).u16(index).u16(0).u16(kReadWhole), 1);
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err != DlpError::None) {
        return err;
    }
    return read_record_reply(response, out);
}

DlpError DlpClient::read_record_by_id(std::uint8_t handle, std::uint32_t recordId, DlpRecord& out)
{
    DlpRequest request(DlpFunc::ReadRecord);
    request.arg(DlpArgWriter().u8(handle).u8(0).u32(recordId).u16(0).u16(kReadWhole));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err != DlpError::None) {
        return err;
    }
    return read_record_reply(response, out);
}

DlpError DlpClient::write_record(std::uint8_t handle, const DlpRecord& record, std::uint32_t* newId)
{
    constexpr std::uint8_t kWriteRecordFlags = 0x80;
    DlpRequest request(DlpFunc::WriteRecord);
    request.arg(DlpArgWriter()
                    .u8(handle)
                    .u8(kWriteRecordFlags)
                    .u32(record.id)
                    .u8(record.attributes)
                    .u8(record.category)
                    .bytes(record.data));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err != DlpError::None) {
        return err;
    }
    DlpArgReader reader(response.arg(0));
    const std::uint32_t id = reader.u32();
    if (newId != nullptr) {
        *newId = id;
    }
    return parse_failure(reader);
}

DlpError DlpClient::delete_record(std::uint8_t handle, std::uint32_t recordId)
{
    DlpRequest request(DlpFunc::DeleteRecord);
    request.arg(DlpArgWriter().u8(handle).u8(0).u32(recordId));
    DlpResponse response;
    return execute(request, response);
}

DlpError DlpClient::read_resource_by_index(std::uint8_t handle, std::uint16_t index, DlpResource& out)
{
    DlpRequest request(DlpFunc::ReadResource);
    request.arg(DlpArgWriter().u8(handle).u8(0).u16(index).u16(0).u16(kReadWhole));
    DlpResponse response;
    const DlpError err = execute(request, response);
    if (err != DlpError::None) {
        return err;
    }
    DlpArgReader reader(response.arg(0));
    read_type_id(reader, out.type);
    out.id = reader.u16();
    reader.skip(2); // index
    const std::uint16_t size = reader.u16();
    out.data = reader.bytes(size);
    return parse_failure(reader);
}

DlpError DlpClient::write_resource(std::uint8_t handle, const DlpResource& resource)
{
    DlpRequest request(DlpFunc::WriteResource);
    request.arg(DlpArgWriter()
                    .u8(handle)
                    .u8(0)
                    .type_id(resource.type)
                    .u16(resource.id)
                    .u16(static_cast<std::uint16_t>(resource.data.size()))
                    .bytes(resource.data));
    DlpResponse response;
    return execute(request, response);
}

DlpError DlpClient::add_sync_log_entry(const std::string& text)
{
    DlpRequest request(DlpFunc::AddSyncLogEntry);
    request.arg(DlpArgWriter().cstring(text));
    DlpResponse response;
    return execute(request, response);
}

DlpError DlpClient::reset_system()
{
    DlpResponse response;
    return execute(DlpRequest(DlpFunc::ResetSystem), response);
}

DlpError DlpClient::end_of_sync(std::uint16_t termCode)
{
    DlpRequest request(DlpFunc::EndOfSync);
    request.arg(DlpArgWriter().u16(termCode));
    DlpResponse response;
    return execute(request, response);
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
