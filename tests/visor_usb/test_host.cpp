#include "doctest.h"
#include "mock_usb.h"
#include "fujinet/platform/posix/visor_usb/ingress.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/devices/clock_device.h"
#include "net_device_test_helpers.h"
#include <thread>
#include "fujinet/io/transport/io_service.h"
using namespace fujinet;
using namespace fujinet::io;
using namespace fujinet::io::protocol;
using namespace fujinet::platform::posix::visor_usb;
using namespace std::chrono_literals;
namespace {
ByteBuffer fixture{0x45,4,6,0,0x4f,0};
void inject(const ByteBuffer& p) { mock_usb::input.push_back({LIBUSB_TRANSFER_COMPLETED,p}); }
void pump(Host& host,unsigned n=8) { while(n--) host.poll(); }
void pump(Ingress& host,unsigned n=8) { while(n--) host.poll(); }
struct Router : IRequestHandler {
    NetworkDevice network{tests::netdev::make_stub_registry_http_only()};
    ClockDevice clock;
    std::vector<const void*> cleanups;
    IOResponse handleRequest(const IORequest& r) override { return r.deviceId==0xfd ? network.handle(r) : clock.handle(r); }
    void resetClient(DeviceID id,const void* owner) override {
        CHECK_FALSE(mock_usb::inCallback); CHECK(id==0xfd); cleanups.push_back(owner); network.resetClient(owner);
    }
};
}
TEST_CASE("Visor host setup rejects invalid discovery without live libusb") {
    mock_usb::reset();
    SUBCASE("absent") { mock_usb::present=false; }
    SUBCASE("wrong VID") { mock_usb::vid=1; }
    SUBCASE("wrong PID") { mock_usb::pid=1; }
    SUBCASE("ambiguous devices") { mock_usb::devices=2; }
    SUBCASE("access") { mock_usb::openError=LIBUSB_ERROR_ACCESS; }
    SUBCASE("busy") { mock_usb::claimError=LIBUSB_ERROR_BUSY; }
    SUBCASE("kernel ownership") { mock_usb::kernel=1; }
    SUBCASE("malformed old info") { mock_usb::info[0]=3; }
    SUBCASE("short connection info") { mock_usb::oldSize=4; }
    SUBCASE("HotSync only") { mock_usb::info[4]=2; }
    SUBCASE("missing Generic2") { mock_usb::inAddress=0x81; mock_usb::outAddress=1; }
    SUBCASE("wrong type") { mock_usb::endpointType=3; }
    SUBCASE("wrong direction") { mock_usb::inAddress=2; }
    SUBCASE("wrong interface class") { mock_usb::interfaceClass=0; }
    SUBCASE("wrong configuration") { mock_usb::config=2; }
    SUBCASE("wrong active alternate") { mock_usb::alternate=1; }
    SUBCASE("disconnect during setup") { mock_usb::extendedError=LIBUSB_ERROR_NO_DEVICE; }
    { Host h; h.poll(); CHECK_FALSE(h.active()); CHECK(h.generation()==0); }
    CHECK(mock_usb::claims==mock_usb::releases); CHECK(mock_usb::pending()==0);
}
TEST_CASE("Visor host uses validated MPS and optional extended-query PIPE fallback") {
    mock_usb::reset();
    SUBCASE("Generic1") { mock_usb::inAddress=0x81; mock_usb::outAddress=1; mock_usb::mps=16; }
    Host h(Options{mock_usb::mps==16 ? 1u:2u}); h.poll(); REQUIRE(h.active());
    CHECK(mock_usb::inLength==mock_usb::mps); CHECK(h.generation()==1);
    inject(fixture); pump(h); ByteBuffer p; REQUIRE(h.take(p)); CHECK(p==fixture); CHECK_FALSE(h.take(p));
    CHECK(h.send(ByteBuffer{0x45,4,0x0d,0,0x48,1,0,1,3,'U','T','C',0}));
    pump(h); CHECK(mock_usb::written.size()==13);
}
TEST_CASE("Visor host refuses ambiguous selection but permits exact physical path") {
    mock_usb::reset(); mock_usb::devices=2;
    Host h(Options{2,1,{9,4}}); h.poll(); CHECK(h.active());
}
TEST_CASE("Visor host handles partial and zero-byte IN without losing byte order") {
    mock_usb::reset(); Host h; h.poll(); REQUIRE(h.active());
    mock_usb::input.push_back({LIBUSB_TRANSFER_COMPLETED,{}});
    mock_usb::input.push_back({LIBUSB_TRANSFER_TIMED_OUT,{fixture.begin(),fixture.begin()+3}});
    mock_usb::input.push_back({LIBUSB_TRANSFER_COMPLETED,{fixture.begin()+3,fixture.end()}});
    pump(h); ByteBuffer p; REQUIRE(h.take(p)); CHECK(p==fixture); CHECK(h.active());
}
TEST_CASE("Visor host retires malformed stream and never accepts a later plausible header") {
    mock_usb::reset(); Host h; h.poll(); auto bad=fixture; bad[4]^=1; inject(bad); inject(fixture);
    pump(h); CHECK_FALSE(h.active()); ByteBuffer p; CHECK_FALSE(h.take(p));
    CHECK(h.status().find("Invalid/overflowed")!=std::string::npos);
    CHECK(mock_usb::pending()==0);
}
TEST_CASE("Visor host retires truncated partial record on bounded deadline") {
    mock_usb::reset(); Host h; h.poll(); inject({fixture.begin(),fixture.begin()+3}); pump(h);
    REQUIRE(h.active()); std::this_thread::sleep_for(5050ms); h.poll(); CHECK_FALSE(h.active());
}
TEST_CASE("Visor host sends successful partial OUT suffix once but never retries ambiguous OUT") {
    mock_usb::reset(); Host h; h.poll(); REQUIRE(h.active());
    SUBCASE("successful short completion") {
        mock_usb::output.push_back({LIBUSB_TRANSFER_COMPLETED,{},2});
        REQUIRE(h.send(fixture)); pump(h); CHECK(h.active()); CHECK(mock_usb::written==fixture); CHECK(mock_usb::outSubmits==2);
    }
    SUBCASE("timeout with partial bytes") {
        mock_usb::output.push_back({LIBUSB_TRANSFER_TIMED_OUT,{},2});
        REQUIRE(h.send(fixture)); pump(h); CHECK_FALSE(h.active()); CHECK(mock_usb::written.size()==2); CHECK(mock_usb::outSubmits==1);
    }
    SUBCASE("timeout with zero bytes") {
        mock_usb::output.push_back({LIBUSB_TRANSFER_TIMED_OUT,{},0});
        REQUIRE(h.send(fixture)); pump(h); CHECK_FALSE(h.active()); CHECK(mock_usb::outSubmits==1);
    }
}
TEST_CASE("Visor host cancellation retains transfer storage until delayed callback") {
    mock_usb::reset();
    { Host h; h.poll(); REQUIRE(h.active()); REQUIRE(h.send(fixture));
      mock_usb::holdCancellation=true; h.retire("test close"); pump(h);
      CHECK_FALSE(h.active()); CHECK(mock_usb::pending()==2); CHECK(mock_usb::releases==0);
      inject(fixture); mock_usb::lateSuccess=true; mock_usb::holdCancellation=false; pump(h);
      ByteBuffer p; CHECK_FALSE(h.take(p)); CHECK(mock_usb::pending()==0); CHECK(mock_usb::releases==1); }
    CHECK(mock_usb::freesPending==0);
}
TEST_CASE("Visor host reconnect requires verified old-device absence and new generation") {
    mock_usb::reset(); Host h; h.poll(); REQUIRE(h.active());
    mock_usb::input.push_back({LIBUSB_TRANSFER_NO_DEVICE,{}}); pump(h); CHECK_FALSE(h.active());
    std::this_thread::sleep_for(1010ms); h.poll(); CHECK_FALSE(h.active()); // same USB identity still listed
    mock_usb::replace_device(); std::this_thread::sleep_for(1010ms); h.poll(); REQUIRE(h.active());
    CHECK(h.generation()==2); CHECK(h.status().find("address=59")!=std::string::npos); ByteBuffer p; CHECK_FALSE(h.take(p)); inject(fixture); pump(h); REQUIRE(h.take(p)); CHECK(p==fixture);
}
TEST_CASE("Visor ingress wraps real Clock codec and publishes ownership without duplicates") {
    mock_usb::reset(); Router router; Ingress ingress(router); pump(ingress); inject(fixture); pump(ingress);
    IORequest req; REQUIRE(ingress.receive(req)); CHECK(req.clientOwner!=nullptr); CHECK(req.deviceId==0x45);
    const auto resp=router.handleRequest(req); REQUIRE(resp.status==StatusCode::Ok);
    ingress.send(resp); pump(ingress); REQUIRE(FujiBusPacket::fromRaw(mock_usb::written));
    CHECK_FALSE(ingress.receive(req));
    std::vector<diag::DiagCommandSpec> commands; ingress.list_commands(commands); REQUIRE(commands.size()==1);
    CHECK(commands[0].safe); CHECK(ingress.execute({"visor.status",{"visor.status"}}).text.find("mps=64")!=std::string::npos);
}
TEST_CASE("Visor ingress owner cleanup closes two handles and preserves another client's handle") {
    mock_usb::reset(); Router router; Ingress ingress(router); pump(ingress);
    auto open=tests::netdev::to_vec(tests::netdev::open_payload("http://example.com/test",1,0,0,{}));
    FujiBusPacket p(WireDeviceId::NetworkService,1); p.setData(open);
    const void* owner=nullptr; std::vector<unsigned> handles;
    for(unsigned i=0;i<2;++i) {
        inject(p.serializeRaw()); pump(ingress); IORequest req; REQUIRE(ingress.receive(req));
        if(owner) CHECK(owner==req.clientOwner); owner=req.clientOwner;
        auto resp=router.handleRequest(req); REQUIRE(resp.status==StatusCode::Ok); REQUIRE(resp.payload.size()>=6);
        handles.push_back(resp.payload[4]|(resp.payload[5]<<8)); ingress.send(resp); pump(ingress);
    }
    int other=0; IORequest req{}; req.deviceId=0xfd; req.command=1; req.payload=open; req.clientOwner=&other;
    auto resp=router.handleRequest(req); REQUIRE(resp.status==StatusCode::Ok);
    auto otherHandle=resp.payload[4]|(resp.payload[5]<<8);
    mock_usb::input.push_back({LIBUSB_TRANSFER_NO_DEVICE,{}}); pump(ingress);
    REQUIRE(router.cleanups.size()==1); CHECK(router.cleanups[0]==owner);
    for(auto handle:handles) CHECK(tests::netdev::info_req(router.network,0xfd,handle).status==StatusCode::InvalidRequest);
    CHECK(tests::netdev::info_req(router.network,0xfd,otherHandle).status==StatusCode::Ok);
}
TEST_CASE("Visor ingress suppresses response and staged request from a retired connection") {
    mock_usb::reset(); Router router; Ingress ingress(router); pump(ingress);
    ByteBuffer both=fixture; both.insert(both.end(),fixture.begin(),fixture.end()); inject(both); pump(ingress);
    IORequest req; REQUIRE(ingress.receive(req)); const void* oldOwner=req.clientOwner; auto resp=router.handleRequest(req);
    mock_usb::input.push_back({LIBUSB_TRANSFER_NO_DEVICE,{}}); ingress.send(resp); pump(ingress);
    CHECK(mock_usb::outSubmits==0); CHECK_FALSE(ingress.receive(req)); REQUIRE(router.cleanups.size()==1);
    mock_usb::replace_device(); std::this_thread::sleep_for(1010ms); pump(ingress);
    inject(fixture); pump(ingress); REQUIRE(ingress.receive(req)); CHECK(req.clientOwner!=oldOwner);
    ingress.send(resp); CHECK(mock_usb::outSubmits==0); // old request ID cannot match new generation
    ingress.send(router.handleRequest(req)); pump(ingress); CHECK(mock_usb::outSubmits==1);
}
TEST_CASE("Visor host enforces output capacity and bounded response queue") {
    mock_usb::reset(); Host h; h.poll(); REQUIRE(h.active());
    SUBCASE("oversized valid packet") {
        FujiBusPacket p(WireDeviceId::Clock,4); p.setData(ByteBuffer(1024,0xdb));
        CHECK_FALSE(h.send(p.serializeRaw())); CHECK_FALSE(h.active());
    }
    SUBCASE("malformed output") { CHECK_FALSE(h.send(ByteBuffer{1,2})); CHECK_FALSE(h.active()); }
    SUBCASE("queue overflow") {
        for(unsigned i=0;i<4;++i) REQUIRE(h.send(fixture));
        CHECK_FALSE(h.send(fixture)); CHECK_FALSE(h.active());
    }
}
TEST_CASE("Visor host input stalls and overflow errors retire the connection") {
    mock_usb::reset(); Host h; h.poll(); REQUIRE(h.active());
    SUBCASE("stall") { mock_usb::input.push_back({LIBUSB_TRANSFER_STALL,{}}); }
    SUBCASE("USB overflow") { mock_usb::input.push_back({LIBUSB_TRANSFER_OVERFLOW,{}}); }
    SUBCASE("impossible actual length") { mock_usb::input.push_back({LIBUSB_TRANSFER_COMPLETED,{},65}); }
    pump(h); CHECK_FALSE(h.active());
}
TEST_CASE("Visor host maximum 1024-byte packets survive real MPS-sized callbacks") {
    mock_usb::reset(); Host h; h.poll(); REQUIRE(h.active());
    FujiBusPacket p(WireDeviceId::Clock,4); p.setData(ByteBuffer(1018,0xc0));
    auto raw=p.serializeRaw(); REQUIRE(raw.size()==1024); inject(raw); pump(h,24);
    ByteBuffer received; REQUIRE(h.take(received)); CHECK(received==raw);
    REQUIRE(h.send(raw)); pump(h); CHECK(mock_usb::written==raw);
}
TEST_CASE("Visor environment selection validates exact values without fallback") {
    unsetenv("FN_VISOR_USB_PORT"); unsetenv("FN_VISOR_USB_BUS"); unsetenv("FN_VISOR_USB_PATH");
    CHECK(options_from_environment().port==2);
    for(auto bad:{"0","3","","2x","-1"}) {
        setenv("FN_VISOR_USB_PORT",bad,1); CHECK_THROWS_AS(options_from_environment(),std::invalid_argument);
    }
    setenv("FN_VISOR_USB_PORT","1",1); setenv("FN_VISOR_USB_BUS","1",1); setenv("FN_VISOR_USB_PATH","9.4",1);
    auto o=options_from_environment(); CHECK(o.port==1); CHECK(o.bus==1); CHECK(o.path==std::vector<std::uint8_t>{9,4});
    setenv("FN_VISOR_USB_PATH","9.",1); CHECK_THROWS_AS(options_from_environment(),std::invalid_argument);
    unsetenv("FN_VISOR_USB_PORT"); unsetenv("FN_VISOR_USB_BUS"); unsetenv("FN_VISOR_USB_PATH");
}
TEST_CASE("Visor host new partial record receives its own deadline after aggregated completion") {
    mock_usb::reset(); Host h; h.poll(); REQUIRE(h.active());
    inject({fixture.begin(),fixture.begin()+3}); pump(h);
    std::this_thread::sleep_for(4000ms);
    ByteBuffer rest(fixture.begin()+3,fixture.end()); rest.insert(rest.end(),fixture.begin(),fixture.begin()+3);
    inject(rest); pump(h); ByteBuffer p; REQUIRE(h.take(p)); CHECK(p==fixture);
    std::this_thread::sleep_for(1100ms); h.poll(); REQUIRE(h.active());
    inject({fixture.begin()+3,fixture.end()}); pump(h); REQUIRE(h.take(p)); CHECK(p==fixture);
}

TEST_CASE("Visor ingress idle contract lets IOService wait and wakes for staged packets and detach") {
    mock_usb::reset(); mock_usb::present = false;
    Router router; Ingress ingress(router); IOService service(router); service.addTransport(&ingress);
    service.serviceOnce();
    CHECK_FALSE(ingress.wait_for_work(0ms));
    const auto before = mock_usb::positiveWaits;
    CHECK_FALSE(service.waitForWork(10ms));
    CHECK(mock_usb::positiveWaits == before + 1);
    CHECK(mock_usb::lastWaitMicros == 10000);
    CHECK_FALSE(service.waitForWork(0ms));
}
TEST_CASE("Visor ingress readiness preserves staged packets and core-thread lifecycle cleanup") {
    mock_usb::reset(); Router router; Ingress ingress(router); IOService service(router); service.addTransport(&ingress);
    pump(ingress); CHECK_FALSE(service.waitForWork(0ms));
    inject(fixture); CHECK(service.waitForWork(10ms));
    ingress.poll(); CHECK(service.waitForWork(0ms)); // NativeFramer now owns it
    service.serviceOnce(); pump(ingress); CHECK_FALSE(service.waitForWork(0ms));
    REQUIRE(mock_usb::written.size() > 0);
    mock_usb::input.push_back({LIBUSB_TRANSFER_NO_DEVICE,{}});
    CHECK(service.waitForWork(10ms)); CHECK(router.cleanups.empty());
    service.serviceOnce(); REQUIRE(router.cleanups.size()==1);
    CHECK_FALSE(service.waitForWork(10ms));
}
TEST_CASE("Visor ingress wakes to rearm a completed partial OUT") {
    mock_usb::reset(); Router router; Ingress ingress(router); pump(ingress);
    inject(fixture); pump(ingress); IORequest req; REQUIRE(ingress.receive(req));
    mock_usb::output.push_back({LIBUSB_TRANSFER_COMPLETED,{},2});
    ingress.send(router.handleRequest(req));
    CHECK(ingress.wait_for_work(10ms));
    CHECK(mock_usb::outSubmits==1);
    ingress.poll(); CHECK(mock_usb::outSubmits==2);
    pump(ingress); CHECK_FALSE(ingress.wait_for_work(0ms));
    REQUIRE(FujiBusPacket::fromRaw(mock_usb::written));
}
TEST_CASE("Visor ingress retires response serialization rejection through the real framer path") {
    mock_usb::reset(); Router router; Ingress ingress(router); pump(ingress);
    inject(fixture); pump(ingress); IORequest req; REQUIRE(ingress.receive(req));
    auto response = router.handleRequest(req);
    SUBCASE("oversized") { response.payload = ByteBuffer(1018,0xdb); }
    SUBCASE("empty serialization from invalid command") { response.payload = ByteBuffer(65535,0); }
    SUBCASE("normal") {
        ingress.send(response); pump(ingress); CHECK(mock_usb::outSubmits==1);
        CHECK(router.cleanups.empty()); return;
    }
    ingress.send(response); pump(ingress);
    CHECK(mock_usb::outSubmits==0); REQUIRE(router.cleanups.size()==1);
    CHECK(ingress.execute({"visor.status",{"visor.status"}}).text.find("active=0")!=std::string::npos);
    inject(fixture); pump(ingress); CHECK_FALSE(ingress.receive(req));
    mock_usb::replace_device(); std::this_thread::sleep_for(1010ms); pump(ingress);
    inject(fixture); pump(ingress); REQUIRE(ingress.receive(req));
    ingress.send(router.handleRequest(req)); pump(ingress); CHECK(mock_usb::outSubmits==1);
}

TEST_CASE("Visor host retries pre-session setup failure on the same device") {
    mock_usb::reset();
    SUBCASE("kernel driver released") { mock_usb::kernel=1; }
    SUBCASE("claim becomes available") { mock_usb::claimError=LIBUSB_ERROR_BUSY; }
    SUBCASE("Palm connection info becomes ready") { mock_usb::oldSize=0; }
    SUBCASE("malformed connection info corrected") { mock_usb::info[0]=3; }
    SUBCASE("active alternate corrected after claim") { mock_usb::alternate=1; }
    Host h; h.poll(); REQUIRE_FALSE(h.active()); CHECK(h.generation()==0);
    CHECK(mock_usb::pending()==0); CHECK(mock_usb::claims==mock_usb::releases);
    mock_usb::kernel=0; mock_usb::claimError=0; mock_usb::oldSize=6;
    mock_usb::info={2,0,0,1,0,2}; mock_usb::alternate=0;
    h.poll(); CHECK_FALSE(h.active()); // retry remains rate-limited
    std::this_thread::sleep_for(1010ms); h.poll(); REQUIRE(h.active());
    CHECK(h.generation()==1); CHECK(mock_usb::outSubmits==0);
    inject(fixture); pump(h); ByteBuffer p; REQUIRE(h.take(p)); CHECK(p==fixture);
}
