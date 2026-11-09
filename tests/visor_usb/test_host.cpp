#include "doctest.h"
#include "mock_usb.h"
#include "fujinet/platform/posix/visor_usb/ingress.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/devices/clock_device.h"
#include "net_device_test_helpers.h"
#include "fake_fs.h"
#include "fujinet/console/console_engine.h"
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <unistd.h>
namespace fujinet::console {
std::unique_ptr<IConsoleTransport> create_console_transport_pty();
}
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/io/devices/fuji_device.h"
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
    { Host h({}, mock_usb::now); h.poll(); CHECK_FALSE(h.active()); CHECK(h.generation()==0); }
    CHECK(mock_usb::claims==mock_usb::releases); CHECK(mock_usb::pending()==0);
}
TEST_CASE("Visor host uses validated MPS and optional extended-query PIPE fallback") {
    mock_usb::reset();
    SUBCASE("Generic1") { mock_usb::inAddress=0x81; mock_usb::outAddress=1; mock_usb::mps=16; }
    Host h(Options{mock_usb::mps==16 ? 1u:2u}, mock_usb::now); h.poll(); REQUIRE(h.active());
    CHECK(mock_usb::inLength==mock_usb::mps); CHECK(h.generation()==1);
    inject(fixture); pump(h); ByteBuffer p; REQUIRE(h.take(p)); CHECK(p==fixture); CHECK_FALSE(h.take(p));
    CHECK(h.send(ByteBuffer{0x45,4,0x0d,0,0x48,1,0,1,3,'U','T','C',0}));
    pump(h); CHECK(mock_usb::written.size()==13);
}
TEST_CASE("Visor host refuses ambiguous selection but permits exact physical path") {
    mock_usb::reset(); mock_usb::devices=2;
    Host h(Options{2,1,{9,4}}, mock_usb::now); h.poll(); CHECK(h.active());
}
TEST_CASE("Visor host handles partial and zero-byte IN without losing byte order") {
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); REQUIRE(h.active());
    mock_usb::input.push_back({LIBUSB_TRANSFER_COMPLETED,{}});
    mock_usb::input.push_back({LIBUSB_TRANSFER_TIMED_OUT,{fixture.begin(),fixture.begin()+3}});
    mock_usb::input.push_back({LIBUSB_TRANSFER_COMPLETED,{fixture.begin()+3,fixture.end()}});
    pump(h); ByteBuffer p; REQUIRE(h.take(p)); CHECK(p==fixture); CHECK(h.active());
}
TEST_CASE("Visor host retires malformed stream and never accepts a later plausible header") {
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); auto bad=fixture; bad[4]^=1; inject(bad); inject(fixture);
    pump(h); CHECK_FALSE(h.active()); ByteBuffer p; CHECK_FALSE(h.take(p));
    CHECK(h.status().find("Invalid/overflowed")!=std::string::npos);
    CHECK(mock_usb::pending()==0);
}
TEST_CASE("Visor host retires truncated partial record on bounded deadline") {
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); inject({fixture.begin(),fixture.begin()+3}); pump(h);
    REQUIRE(h.active()); mock_usb::advance(4999ms); h.poll(); REQUIRE(h.active());
    mock_usb::advance(1ms); h.poll(); CHECK_FALSE(h.active());
}
TEST_CASE("Visor host sends successful partial OUT suffix once but never retries ambiguous OUT") {
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); REQUIRE(h.active());
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
    { Host h({}, mock_usb::now); h.poll(); REQUIRE(h.active()); REQUIRE(h.send(fixture));
      mock_usb::holdCancellation=true; h.retire("test close"); pump(h);
      CHECK_FALSE(h.active()); CHECK(mock_usb::pending()==2); CHECK(mock_usb::releases==0);
      inject(fixture); mock_usb::lateSuccess=true; mock_usb::holdCancellation=false; pump(h);
      ByteBuffer p; CHECK_FALSE(h.take(p)); CHECK(mock_usb::pending()==0); CHECK(mock_usb::releases==1); }
    CHECK(mock_usb::freesPending==0);
}
TEST_CASE("Visor host reconnect requires verified old-device absence and new generation") {
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); REQUIRE(h.active());
    mock_usb::input.push_back({LIBUSB_TRANSFER_NO_DEVICE,{}}); pump(h); CHECK_FALSE(h.active());
    mock_usb::advance(1000ms); h.poll(); CHECK_FALSE(h.active()); // same USB identity still listed
    mock_usb::replace_device(); mock_usb::advance(1000ms); h.poll(); REQUIRE(h.active());
    CHECK(h.generation()==2); CHECK(h.status().find("address=59")!=std::string::npos); ByteBuffer p; CHECK_FALSE(h.take(p)); inject(fixture); pump(h); REQUIRE(h.take(p)); CHECK(p==fixture);
}
TEST_CASE("Visor ingress wraps real Clock codec and publishes ownership without duplicates") {
    mock_usb::reset(); Router router; Ingress ingress(router, {}, nullptr, mock_usb::now); pump(ingress); inject(fixture); pump(ingress);
    IORequest req; REQUIRE(ingress.receive(req)); CHECK(req.clientOwner!=nullptr); CHECK(req.deviceId==0x45);
    const auto resp=router.handleRequest(req); REQUIRE(resp.status==StatusCode::Ok);
    ingress.send(resp); pump(ingress); REQUIRE(FujiBusPacket::fromRaw(mock_usb::written));
    CHECK_FALSE(ingress.receive(req));
    std::vector<diag::DiagCommandSpec> commands; ingress.list_commands(commands); REQUIRE(commands.size()==4);
    CHECK(commands[0].safe); CHECK(ingress.execute({"visor.status",{"visor.status"}}).text.find("mps=64")!=std::string::npos);
}
TEST_CASE("Visor ingress owner cleanup closes two handles and preserves another client's handle") {
    mock_usb::reset(); Router router; Ingress ingress(router, {}, nullptr, mock_usb::now); pump(ingress);
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
    mock_usb::reset(); Router router; Ingress ingress(router, {}, nullptr, mock_usb::now); pump(ingress);
    ByteBuffer both=fixture; both.insert(both.end(),fixture.begin(),fixture.end()); inject(both); pump(ingress);
    IORequest req; REQUIRE(ingress.receive(req)); const void* oldOwner=req.clientOwner; auto resp=router.handleRequest(req);
    mock_usb::input.push_back({LIBUSB_TRANSFER_NO_DEVICE,{}}); ingress.send(resp); pump(ingress);
    CHECK(mock_usb::outSubmits==0); CHECK_FALSE(ingress.receive(req)); REQUIRE(router.cleanups.size()==1);
    mock_usb::replace_device(); mock_usb::advance(1000ms); pump(ingress);
    inject(fixture); pump(ingress); REQUIRE(ingress.receive(req)); CHECK(req.clientOwner!=oldOwner);
    ingress.send(resp); CHECK(mock_usb::outSubmits==0); // old request ID cannot match new generation
    ingress.send(router.handleRequest(req)); pump(ingress); CHECK(mock_usb::outSubmits==1);
}
TEST_CASE("Visor host enforces output capacity and bounded response queue") {
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); REQUIRE(h.active());
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
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); REQUIRE(h.active());
    SUBCASE("stall") { mock_usb::input.push_back({LIBUSB_TRANSFER_STALL,{}}); }
    SUBCASE("USB overflow") { mock_usb::input.push_back({LIBUSB_TRANSFER_OVERFLOW,{}}); }
    SUBCASE("impossible actual length") { mock_usb::input.push_back({LIBUSB_TRANSFER_COMPLETED,{},65}); }
    pump(h); CHECK_FALSE(h.active());
}
TEST_CASE("Visor host maximum 1024-byte packets survive real MPS-sized callbacks") {
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); REQUIRE(h.active());
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
    mock_usb::reset(); Host h({}, mock_usb::now); h.poll(); REQUIRE(h.active());
    inject({fixture.begin(),fixture.begin()+3}); pump(h);
    mock_usb::advance(4000ms);
    ByteBuffer rest(fixture.begin()+3,fixture.end()); rest.insert(rest.end(),fixture.begin(),fixture.begin()+3);
    inject(rest); pump(h); ByteBuffer p; REQUIRE(h.take(p)); CHECK(p==fixture);
    mock_usb::advance(1100ms); h.poll(); REQUIRE(h.active());
    inject({fixture.begin()+3,fixture.end()}); pump(h); REQUIRE(h.take(p)); CHECK(p==fixture);
}

TEST_CASE("Visor ingress idle contract lets IOService wait and wakes for staged packets and detach") {
    mock_usb::reset(); mock_usb::present = false;
    Router router; Ingress ingress(router, {}, nullptr, mock_usb::now); IOService service(router); service.addTransport(&ingress);
    service.serviceOnce();
    CHECK_FALSE(ingress.wait_for_work(0ms));
    const auto before = mock_usb::positiveWaits;
    CHECK_FALSE(service.waitForWork(10ms));
    CHECK(mock_usb::positiveWaits == before + 1);
    CHECK(mock_usb::lastWaitMicros == 10000);
    CHECK_FALSE(service.waitForWork(0ms));
}
TEST_CASE("Visor ingress readiness preserves staged packets and core-thread lifecycle cleanup") {
    mock_usb::reset(); Router router; Ingress ingress(router, {}, nullptr, mock_usb::now); IOService service(router); service.addTransport(&ingress);
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
    mock_usb::reset(); Router router; Ingress ingress(router, {}, nullptr, mock_usb::now); pump(ingress);
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
    mock_usb::reset(); Router router; Ingress ingress(router, {}, nullptr, mock_usb::now); pump(ingress);
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
    mock_usb::replace_device(); mock_usb::advance(1000ms); pump(ingress);
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
    Host h({}, mock_usb::now); h.poll(); REQUIRE_FALSE(h.active()); CHECK(h.generation()==0);
    CHECK(mock_usb::pending()==0); CHECK(mock_usb::claims==mock_usb::releases);
    CHECK(mock_usb::opens==mock_usb::closes); CHECK(mock_usb::references==0);
    mock_usb::kernel=0; mock_usb::claimError=0; mock_usb::oldSize=6;
    mock_usb::info={2,0,0,1,0,2}; mock_usb::alternate=0;
    h.poll(); CHECK_FALSE(h.active()); // retry remains rate-limited
    mock_usb::advance(1000ms); h.poll(); REQUIRE(h.active());
    CHECK(h.generation()==1); CHECK(mock_usb::outSubmits==0);
    inject(fixture); pump(h); ByteBuffer p; REQUIRE(h.take(p)); CHECK(p==fixture);
}

TEST_CASE("Visor host sends no vendor request to a device another driver owns") {
    mock_usb::reset(); mock_usb::kernel = 1;
    Host h({}, mock_usb::now); h.poll();
    for (int i = 0; i < 5; ++i) { mock_usb::advance(1000ms); h.poll(); }
    REQUIRE_FALSE(h.active());
    CHECK(mock_usb::controlTransfers == 0);
    CHECK(mock_usb::claims == mock_usb::releases); CHECK(mock_usb::opens == mock_usb::closes);
    mock_usb::kernel = 0; mock_usb::advance(1000ms); h.poll();
    REQUIRE(h.active()); CHECK(mock_usb::controlTransfers == 3); // the two queries and the alternate check, once it is free
}

namespace {
// Restore the caller's environment even when a REQUIRE aborts a test.
struct SelectionEnvironment {
    struct Variable { const char* name; bool present; std::string value; };
    std::vector<Variable> variables;
    SelectionEnvironment() {
        for (const auto* name : {"FN_VISOR_USB_PORT", "FN_VISOR_USB_BUS", "FN_VISOR_USB_PATH"}) {
            const char* value = std::getenv(name);
            variables.push_back({name, value != nullptr, value ? value : ""}); unsetenv(name);
        }
    }
    ~SelectionEnvironment() {
        for (const auto& v : variables) {
            if (v.present) setenv(v.name, v.value.c_str(), 1); else unsetenv(v.name);
        }
    }
};
}
TEST_CASE("Visor selection resolves config then strict environment overrides") {
    SelectionEnvironment environment;
    config::VisorConfig stored{1, 2, "9.4"};
    auto o=resolve_options(stored);
    CHECK(o.port==1); CHECK(o.bus==2); CHECK(o.path==std::vector<std::uint8_t>{9,4});
    CHECK(o.portSource=="config"); CHECK(o.busSource=="config"); CHECK(o.pathSource=="config");
    setenv("FN_VISOR_USB_PORT","2",1); setenv("FN_VISOR_USB_BUS","1",1); setenv("FN_VISOR_USB_PATH","9.5",1);
    o=resolve_options(stored);
    CHECK(o.port==2); CHECK(o.bus==1); CHECK(o.path==std::vector<std::uint8_t>{9,5});
    CHECK(o.portSource=="environment"); CHECK(o.busSource=="environment"); CHECK(o.pathSource=="environment");
    for (const auto* bad : {"0", "3", "", "2x", "-1"}) {
        setenv("FN_VISOR_USB_PORT",bad,1); CHECK_THROWS_AS(resolve_options(stored),config::InvalidVisorConfig);
    }
    setenv("FN_VISOR_USB_PORT","2",1);
    setenv("FN_VISOR_USB_PATH","",1); CHECK_THROWS_AS(resolve_options(stored),config::InvalidVisorConfig);
    setenv("FN_VISOR_USB_PATH","9.4",1);
    for (const auto* bad : {"0", "256", "-1", "", "1x"}) {
        setenv("FN_VISOR_USB_BUS",bad,1); CHECK_THROWS_AS(resolve_options(stored),config::InvalidVisorConfig);
    }
    setenv("FN_VISOR_USB_BUS","1",1);
    for (const auto* bad : {"9.", ".9", "9..4", "0", "256", "1.2.3.4.5.6.7.8"}) {
        setenv("FN_VISOR_USB_PATH",bad,1); CHECK_THROWS_AS(resolve_options(stored),config::InvalidVisorConfig);
    }
    setenv("FN_VISOR_USB_PATH","9.4",1);
    stored.port=3; CHECK_THROWS_AS(resolve_options(stored),config::InvalidVisorConfig); // bad config cannot be hidden
}
TEST_CASE("Visor console config get set save preserves live selection and environment provenance") {
    SelectionEnvironment environment; mock_usb::reset();
    tests::MemoryFileSystem fs("primary");
    auto store=std::make_unique<config::YamlFujiConfigStoreFs>(&fs,nullptr,"fujinet.yaml");
    io::FujiDevice fuji(nullptr,std::move(store)); fuji.start();
    setenv("FN_VISOR_USB_PORT","2",1);
    Router router; Ingress ingress(router,resolve_options(fuji.config().visor),&fuji,mock_usb::now);
    pump(ingress); REQUIRE(ingress.execute({"visor.status",{"visor.status"}}).text.find("active=1")!=std::string::npos);
    const auto run=[&](std::initializer_list<std::string_view> words) {
        diag::DiagArgsView args; args.argv=words; return ingress.execute(args);
    };
    auto status=run({"visor.status"}).text;
    CHECK(status.find("selection_port=2 source=environment")!=std::string::npos);
    CHECK(status.find("selection_bus=unset source=config")!=std::string::npos);
    CHECK(status.find("selection_path=unset source=config")!=std::string::npos);
    CHECK(run({"visor.get"}).text.find("port=2 bus=unset path=unset")!=std::string::npos);
    for (auto args : {std::vector<std::string_view>{"visor.set","port","0"}, {"visor.set","bus","256"},
            {"visor.set","path","9.4"}, {"visor.set","port"}, {"visor.get","extra"}}) {
        CHECK(ingress.execute({"",args}).status==diag::DiagStatus::InvalidArgs);
    }
    CHECK(run({"visor.set","port","1"}).status==diag::DiagStatus::Ok);
    CHECK(run({"visor.set","bus","1"}).status==diag::DiagStatus::Ok);
    CHECK(run({"visor.set","path","9.4"}).status==diag::DiagStatus::Ok);
    CHECK(run({"visor.set","bus","unset"}).status==diag::DiagStatus::InvalidArgs);
    CHECK(run({"visor.get"}).text.find("port=1 bus=1 path=9.4")!=std::string::npos);
    CHECK(run({"visor.save"}).status==diag::DiagStatus::Ok);
    const auto saved=fuji.config_store()->load();
    CHECK(saved.visor.port==1); CHECK(saved.visor.bus==1); CHECK(saved.visor.path=="9.4");
    // Environment is neither persisted nor changed, and no session is disrupted.
    status=run({"visor.status"}).text;
    CHECK(status.find("selection_port=2 source=environment")!=std::string::npos);
    CHECK(status.find("selection_bus=unset source=config")!=std::string::npos);
    CHECK(status.find("active=1 generation=1")!=std::string::npos);
    CHECK(mock_usb::outSubmits==0);
    CHECK(run({"visor.set","path","unset"}).status==diag::DiagStatus::Ok);
    CHECK(run({"visor.set","bus","unset"}).status==diag::DiagStatus::Ok);
    CHECK(run({"visor.save"}).status==diag::DiagStatus::Ok);
    CHECK(fuji.config_store()->load().visor.path.empty());
    // A newly created application/config store sees the saved values.
    config::YamlFujiConfigStoreFs reopened(&fs,nullptr,"fujinet.yaml");
    CHECK(reopened.load().visor.port==1);
}

TEST_CASE("Visor host output deadline is exact and never replays a pending write") {
    mock_usb::reset(); Host h({},mock_usb::now); h.poll(); REQUIRE(h.active());
    mock_usb::holdOutput=true; REQUIRE(h.send(fixture));
    mock_usb::advance(4999ms); h.poll(); REQUIRE(h.active()); CHECK(mock_usb::outSubmits==1);
    mock_usb::advance(1ms); h.poll(); CHECK_FALSE(h.active());
    pump(h); CHECK(mock_usb::pending()==0); CHECK(mock_usb::freesPending==0);
    CHECK(mock_usb::outSubmits==1); CHECK(mock_usb::written.empty());
    mock_usb::holdOutput=false; mock_usb::advance(1000ms); h.poll(); CHECK_FALSE(h.active());
    CHECK(h.generation()==1); // timed-out physical identity remains quarantined
}

TEST_CASE("Visor console PTY serves settings while mock USB routes Clock") {
    SelectionEnvironment environment; mock_usb::reset();
    tests::MemoryFileSystem fs("primary");
    auto store=std::make_unique<config::YamlFujiConfigStoreFs>(&fs,nullptr,"fujinet.yaml");
    io::FujiDevice fuji(nullptr,std::move(store)); fuji.start();
    Router router; Ingress ingress(router,resolve_options(fuji.config().visor),&fuji,mock_usb::now);
    pump(ingress);
    std::ostringstream announcement;
    struct Capture {
        std::streambuf* saved;
        explicit Capture(std::ostream& target) : saved(std::cout.rdbuf(target.rdbuf())) {}
        ~Capture() { std::cout.rdbuf(saved); }
    };
    std::unique_ptr<console::IConsoleTransport> transport;
    { Capture capture(announcement); transport=console::create_console_transport_pty(); }
    REQUIRE(transport);
    auto text=announcement.str();
    const auto prefix=text.find("console to: "); REQUIRE(prefix!=std::string::npos);
    const auto path=text.substr(prefix+12, text.find('\n',prefix)-(prefix+12));
    struct Fd { int fd; ~Fd() { if(fd>=0) ::close(fd); } } slave{::open(path.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK)};
    REQUIRE(slave.fd>=0);
    diag::DiagnosticRegistry registry; registry.add_provider(ingress);
    console::ConsoleEngine engine(registry,*transport);
    const std::string commands="visor.set port 1\nvisor.save\nvisor.get\nvisor.status\n";
    REQUIRE(::write(slave.fd,commands.data(),commands.size())==static_cast<ssize_t>(commands.size()));
    inject(fixture);
    std::string output;
    for(unsigned i=0;i<256;++i) {
        REQUIRE(engine.step(0));
        ingress.poll(); IORequest request;
        if(ingress.receive(request)) ingress.send(router.handleRequest(request));
        char buffer[2048]; ssize_t n;
        while((n=::read(slave.fd,buffer,sizeof(buffer)))>0) output.append(buffer,n);
        CHECK(((n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) || n==0));
    }
    CHECK(output.find("Configured; visor.save to persist")!=std::string::npos);
    CHECK(output.find("Saved visor settings")!=std::string::npos);
    CHECK(output.find("port=1 bus=unset path=unset")!=std::string::npos);
    CHECK(output.find("selection_port=2 source=config")!=std::string::npos);
    CHECK(output.find("active=1 generation=1")!=std::string::npos);
    CHECK(fuji.config_store()->load().visor.port==1);
    REQUIRE(FujiBusPacket::fromRaw(mock_usb::written));
    CHECK(mock_usb::outSubmits==1);
}
