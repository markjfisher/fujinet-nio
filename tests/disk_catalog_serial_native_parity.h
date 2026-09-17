#pragma once

#include "doctest.h"
#include "disk_serial_native_parity.h"
#include "fujinet/io/devices/app_store.h"
#include "fujinet/io/devices/slot_catalog_service.h"

namespace catalog_parity {
using Bytes = std::vector<std::uint8_t>;
using disk_parity::BytePipeChannel;
using disk_parity::PacketChannel;
using disk_parity::PacketIODouble;
using fujinet::io::IORequest;
using fujinet::io::IOResponse;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FujiBusPacket;
using fujinet::io::protocol::WireDeviceId;

// Local adaptation: both production framers, a real VirtualDevice, and an
// explicit endpoint. No disk-specific response oracles are reused.
inline IOResponse exchange(bool native, fujinet::io::VirtualDevice& service,
                           std::uint8_t endpoint, std::uint8_t command, const Bytes& payload)
{
    BytePipeChannel serial;
    PacketIODouble packets(4096, 4, 8192);
    PacketChannel packetChannel(&packets);
    fujinet::io::SlipFramer slip;
    fujinet::io::NativeFramer packetFramer;
    fujinet::io::Channel& channel = native
        ? static_cast<fujinet::io::Channel&>(packetChannel) : serial;
    fujinet::io::FujiBusTransport transport(channel, native
        ? static_cast<fujinet::io::IFramer&>(packetFramer) : slip);
    FujiBusPacket packet(static_cast<WireDeviceId>(endpoint), command);
    packet.setData(payload);
    if (native) REQUIRE(packets.enqueue(packet.serializeRaw()));
    else serial.push(packet.serialize());
    transport.poll();
    IORequest request{};
    REQUIRE(transport.receive(request));
    REQUIRE(request.deviceId == endpoint);
    REQUIRE(request.command == command);
    REQUIRE(request.payload == payload);
    transport.send(service.handle(request));
    if (native) {
        Bytes reply;
        REQUIRE(packets.takeSent(reply));
        REQUIRE(packets.enqueue(reply));
    } else serial.push(serial.take_tx());
    transport.poll();
    IOResponse reply{};
    REQUIRE(transport.receiveResponse(reply));
    transport.poll();
    IOResponse extra{};
    CHECK_FALSE(transport.receiveResponse(extra));
    if (native) {
        Bytes extraWire;
        CHECK_FALSE(packets.takeSent(extraWire));
    } else CHECK(serial.take_tx().empty());
    return reply;
}

inline void lp(Bytes& p, const std::string& s)
{
    p.push_back(static_cast<std::uint8_t>(s.size()));
    p.push_back(static_cast<std::uint8_t>(s.size() >> 8));
    p.insert(p.end(), s.begin(), s.end());
}
inline Bytes put(std::uint8_t index, bool ro, const std::string& uri)
{
    Bytes p{1, index, static_cast<std::uint8_t>(ro ? 2 : 0)};
    lp(p, uri);
    return p;
}
inline Bytes entry(std::uint8_t index, bool ro, const std::string& uri)
{
    Bytes p{1, static_cast<std::uint8_t>(ro ? 3 : 1), index};
    lp(p, uri);
    return p;
}
inline Bytes mount(std::uint8_t slot, bool ro, const std::string& uri)
{
    // Auto detection and deliberately no sector hint: geometry comes from ADF.
    Bytes p{1, slot, static_cast<std::uint8_t>(ro), 0, 0, 0};
    lp(p, uri);
    return p;
}
inline Bytes geometry(std::uint8_t slot, std::uint8_t flags, bool hd, bool info = false,
                      std::uint8_t error = 0)
{
    Bytes p{1, flags, 0, 0, slot, 4, 0, 2,
            static_cast<std::uint8_t>(hd ? 0xc0 : 0xe0),
            static_cast<std::uint8_t>(hd ? 0x0d : 0x06), 0, 0};
    if (info) p.push_back(error);
    return p;
}
inline Bytes empty_info(std::uint8_t slot, std::uint8_t flags = 0x28)
{
    return {1, flags, 0, 0, slot, 0, 0, 0, 0, 0, 0, 0, 0};
}
inline Bytes sector_request(std::uint8_t slot) { return {1, slot, 3, 0, 0, 0, 0, 2}; }
inline Bytes sector_reply(std::uint8_t slot) { return {1, 0, 0, 0, slot, 3, 0, 0, 0, 0, 2}; }
inline Bytes seed(bool hd, std::uint8_t marker)
{
    Bytes bytes((hd ? 3520u : 1760u) * 512u);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(marker + i / 512 + i % 251);
    bytes[0] = 'D'; bytes[1] = 'O'; bytes[2] = 'S'; bytes[3] = marker & 1;
    return bytes;
}

class World {
public:
    explicit World(bool native) : native(native)
    {
        auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("host");
        fs = owned.get();
        REQUIRE(fs->createDirectory("/disks"));
        REQUIRE(fs->create_file("/disks/a.adf", seed(false, 0x40)));
        REQUIRE(fs->create_file("/disks/b.adf", seed(false, 0x61)));
        REQUIRE(fs->create_file("/disks/hd.adf", seed(true, 0x82)));
        REQUIRE(fs->create_file("/disks/bad.adf", Bytes(513, 0)));
        REQUIRE(storage.registerFileSystem(std::move(owned)));
        recreate();
    }
    void recreate()
    {
        catalog.reset(); disk.reset(); store.reset();
        store = std::make_shared<fujinet::io::AppStore>(storage);
        catalog = std::make_unique<fujinet::io::SlotCatalogService>(storage, store);
        disk = std::make_unique<fujinet::io::DiskDevice>(storage);
    }
    IOResponse expect(std::uint8_t endpoint, std::uint8_t command, const Bytes& input,
                     StatusCode status, const Bytes& expected)
    {
        INFO("native=" << native << " endpoint=" << unsigned(endpoint)
             << " command=" << unsigned(command));
        auto& service = endpoint == 0xf2
            ? static_cast<fujinet::io::VirtualDevice&>(*catalog)
            : static_cast<fujinet::io::VirtualDevice&>(*disk);
        const auto response = exchange(native, service, endpoint, command, input);
        CHECK(response.deviceId == endpoint);
        CHECK(response.command == command);
        REQUIRE(response.status == status);
        REQUIRE(response.payload == expected);
        return response;
    }
    void info(std::uint8_t slot, const Bytes& expected)
    { expect(0xfc, 5, {1, slot}, StatusCode::Ok, expected); }
    void simple(std::uint8_t command, std::uint8_t slot)
    { expect(0xfc, command, {1, slot}, StatusCode::Ok, {1, 0, 0, 0, slot}); }
    void select(std::uint8_t index, std::uint8_t slot, bool ro, const std::string& uri, bool hd)
    {
        const auto selected = expect(0xf2, 1, {1, index}, StatusCode::Ok, entry(index, ro, uri));
        // Model a client selecting the returned catalogue entry, not a new ABI.
        const std::string selectedUri(selected.payload.begin() + 5, selected.payload.end());
        expect(0xfc, 1, mount(slot, (selected.payload[1] & 2) != 0, selectedUri),
               StatusCode::Ok, geometry(slot, ro ? 3 : 1, hd));
    }
    void read(std::uint8_t slot, const Bytes& image, std::uint32_t lba = 3)
    {
        auto expected = sector_reply(slot);
        auto request = sector_request(slot);
        for (unsigned i = 0; i < 4; ++i) {
            expected[5 + i] = static_cast<std::uint8_t>(lba >> (8 * i));
            request[2 + i] = static_cast<std::uint8_t>(lba >> (8 * i));
        }
        expected.insert(expected.end(), image.begin() + lba * 512, image.begin() + (lba + 1) * 512);
        expect(0xfc, 3, request, StatusCode::Ok, expected);
    }
    void backing(const Bytes& a = seed(false, 0x40))
    {
        CHECK(fs->file_bytes("/disks/a.adf") == a);
        CHECK(fs->file_bytes("/disks/b.adf") == seed(false, 0x61));
        CHECK(fs->file_bytes("/disks/hd.adf") == seed(true, 0x82));
        CHECK(fs->file_bytes("/disks/bad.adf") == Bytes(513, 0));
    }
    void runtime(const std::string& expected)
    {
        REQUIRE(fs->exists("/fujinet-runtime-mounts.tsv"));
        const auto& bytes = fs->file_bytes("/fujinet-runtime-mounts.tsv");
        CHECK(std::string(bytes.begin(), bytes.end()) == expected);
    }
    bool native;
    fujinet::fs::StorageManager storage;
    fujinet::tests::MemoryFileSystem* fs{};
    std::shared_ptr<fujinet::io::AppStore> store;
    std::unique_ptr<fujinet::io::SlotCatalogService> catalog;
    std::unique_ptr<fujinet::io::DiskDevice> disk;
};
} // namespace catalog_parity
