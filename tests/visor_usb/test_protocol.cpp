#include "doctest.h"
#include "fujinet/io/visor_usb/protocol.h"
#include <algorithm>
using namespace fujinet::io;
using namespace fujinet::io::visor_usb;
using namespace fujinet::io::protocol;
namespace {
ByteBuffer packet(std::size_t n) {
    FujiBusPacket p(WireDeviceId::Clock, 4);
    if(n>6) { ByteBuffer data(n-6); for(std::size_t i=0;i<data.size();++i) data[i]=std::uint8_t(i); p.setData(std::move(data)); }
    return p.serializeRaw();
}
void checksum(ByteBuffer& p) {
    p[4]=0; unsigned c=0; for(auto b:p) { c+=b; c=(c>>8)+(c&255); } p[4]=c;
}
}
TEST_CASE("Visor protocol validates old connection info and Generic endpoint identity") {
    auto ports=connection_info(ByteBuffer{2,0,0,1,0,2}); REQUIRE(ports);
    std::vector<Endpoint> eps{{1,0,0,255,0x82,2,64},{1,0,0,255,2,2,64}};
    CHECK(select_generic_pair(*ports,eps,2)); CHECK_FALSE(select_generic_pair(*ports,eps,1));
    SUBCASE("HotSync is not Generic") { (*ports)[1].function=2; CHECK_FALSE(select_generic_pair(*ports,eps,2)); }
    SUBCASE("duplicate pair") { eps.push_back(eps.back()); CHECK_FALSE(select_generic_pair(*ports,eps,2)); }
    SUBCASE("wrong type") { eps[0].type=3; CHECK_FALSE(select_generic_pair(*ports,eps,2)); }
    SUBCASE("wrong direction") { eps[0].address=2; CHECK_FALSE(select_generic_pair(*ports,eps,2)); }
    SUBCASE("wrong class") { eps[0].interfaceClass=2; CHECK_FALSE(select_generic_pair(*ports,eps,2)); }
    SUBCASE("zero/oversized MPS") { eps[0].mps=0; CHECK_FALSE(select_generic_pair(*ports,eps,2)); eps[0].mps=512; CHECK_FALSE(select_generic_pair(*ports,eps,2)); eps[0].mps=3; CHECK_FALSE(select_generic_pair(*ports,eps,2)); }
    SUBCASE("wrong config/alternate/interface") {
        eps[0].configuration=2; CHECK_FALSE(select_generic_pair(*ports,eps,2));
        eps[0].configuration=1; eps[0].alternate=1; CHECK_FALSE(select_generic_pair(*ports,eps,2));
        eps[0].alternate=0; eps[0].interface=1; CHECK_FALSE(select_generic_pair(*ports,eps,2));
    }
    for(auto bad:std::vector<ByteBuffer>{{},{2,0,0,1},{3,0,0,1,0,2},{2,1,0,1,0,2},{2,0,0,1,0,1},{2,0,5,1,0,2},{2,0,0,1,0,3}})
        CHECK_FALSE(connection_info(bad));
}
TEST_CASE("Visor raw packets preserve arbitrary USB boundaries and binary data") {
    for(std::size_t n:{6,16,64,65,1024}) {
        auto p=packet(n); REQUIRE(p.size()==n); REQUIRE(FujiBusPacket::fromRaw(p));
        for(std::size_t split=0;split<=p.size();++split) {
            RawPackets raw; ByteBuffer out;
            REQUIRE(raw.append(std::span(p).first(split)));
            REQUIRE(raw.append(std::span(p).subspan(split)));
            REQUIRE(raw.pop(out)); REQUIRE(out==p); REQUIRE_FALSE(raw.pop(out));
        }
    }
    auto p=packet(1024); auto parsed=FujiBusPacket::fromRaw(p); REQUIRE(parsed); REQUIRE(parsed->data());
    for(auto b:{0,0xc0,0xdb}) CHECK(std::find(parsed->data()->begin(),parsed->data()->end(),b)!=parsed->data()->end());
}
TEST_CASE("Visor raw aggregated records retain excess and never duplicate") {
    auto p=packet(16), q=packet(64); ByteBuffer both=p; both.insert(both.end(),q.begin(),q.end());
    RawPackets raw; REQUIRE(raw.append(both)); ByteBuffer out;
    REQUIRE(raw.pop(out)); CHECK(out==p); REQUIRE(raw.pop(out)); CHECK(out==q); CHECK_FALSE(raw.pop(out));
}
TEST_CASE("Visor raw corruption poisons the stream without unsafe resynchronization") {
    auto p=packet(16);
    SUBCASE("bad checksum") { p[4]^=1; }
    SUBCASE("bad descriptor") { p.resize(6); p[2]=6; p[5]=0x80; checksum(p); }
    SUBCASE("too short") { p[2]=5; checksum(p); }
    SUBCASE("oversized") { p[2]=1; p[3]=4; checksum(p); }
    RawPackets raw; CHECK_FALSE(raw.append(p)); CHECK(raw.faulted());
    CHECK_FALSE(raw.append(packet(6))); ByteBuffer out; CHECK_FALSE(raw.pop(out));
}
TEST_CASE("Visor raw partial packets remain partial and bounded queue overflow is fatal") {
    RawPackets raw; auto p=packet(64); REQUIRE(raw.append(std::span(p).first(5))); CHECK(raw.partial());
    ByteBuffer out; CHECK_FALSE(raw.pop(out));
    raw.clear();
    for(int i=0;i<4;++i) REQUIRE(raw.append(packet(6)));
    CHECK_FALSE(raw.append(packet(6))); CHECK(raw.faulted()); CHECK_FALSE(raw.pop(out));
}
