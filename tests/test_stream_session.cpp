#include "doctest.h"
#include "fujinet/io/stream/session.h"
#include "fujinet/io/atari_sio_nio_frontend.h"
using namespace fujinet::io;
namespace {
struct Router: IRequestHandler {
    std::vector<IORequest> calls;
    bool busy=false, malformed=false;
    IOResponse handleRequest(const IORequest& q) override {
        calls.push_back(q); auto r=make_success_response(q);
        if(q.command==1) r.payload={1,1,0,0,9,0,7};
        if(q.command==2 || q.command==3) {
            if(busy) {r.status=StatusCode::NotReady;return r;}
            r.payload={1,0,0,0,9,0,q.payload[3],q.payload[4],q.payload[5],q.payload[6],1,0};
            if(q.command==2) r.payload.push_back('X');
            if(malformed) r.payload[10]=121;
        }
        return r;
    }
};
std::vector<std::uint8_t> request(const stream::Session& s, unsigned op, unsigned seq,
                                 std::vector<std::uint8_t> data={},unsigned cap=0) {
    auto q=s.query(); std::vector<std::uint8_t> r{1,static_cast<std::uint8_t>(op)};
    r.insert(r.end(),q.begin()+2,q.begin()+6);
    for(unsigned i=0;i<4;++i) r.push_back(seq>>(8*i));
    r.push_back(op==1||op==3?data.size():cap);r.insert(r.end(),data.begin(),data.end());return r;
}
void open(stream::Session& s) {
    REQUIRE(s.execute(request(s,1,0,{'t','c','p',':','/','/','h',':','9'}))[0]==0);
}
}
TEST_CASE("small stream routes offsets and replays lost read and partial write results") {
    Router r; stream::Session s(r,&r); open(s);
    const auto read=request(s,2,1,{},3);
    const auto first=s.execute(read);
    REQUIRE(first.size()==12); CHECK(first[2]==1); CHECK(first[11]=='X');
    CHECK(s.execute(read)==first); CHECK(r.calls.size()==2);
    auto changed=read;changed[10]=4; CHECK(s.execute(changed)[0]==2);CHECK(r.calls.size()==2);
    const auto write=request(s,3,2,{'a','b'});
    auto written=s.execute(write);CHECK(written[2]==1);
    CHECK(s.execute(write)==written); CHECK(r.calls.size()==3);
    CHECK(s.execute(request(s,3,3,{'b'}))[0]==0);
    CHECK(r.calls.back().payload==std::vector<std::uint8_t>{1,9,0,1,0,0,0,1,0,'b'});
    CHECK(s.execute(request(s,2,4,{},2))[0]==0);
    CHECK(r.calls.back().payload==std::vector<std::uint8_t>{1,9,0,1,0,0,0,2,0});
}
TEST_CASE("small stream error replies remain replayable and tokens are checked") {
    Router r;stream::Session s(r,&r);open(s);
    auto read=request(s,2,1,{},5); r.busy=true;
    CHECK(s.execute(read)[0]==4); r.busy=false;
    CHECK(s.execute(read)[0]==4);CHECK(r.calls.size()==2);
    auto bad=request(s,2,2,{},5);bad[2]^=128;
    CHECK(s.execute(bad)[0]==2);CHECK(r.calls.size()==2);
    CHECK(s.execute(request(s,2,2,{},5))[0]==0);CHECK(r.calls.size()==3);
    CHECK(s.execute(read)[0]==2); // old result no longer retained
}
TEST_CASE("small stream rejects malformed requests and poisons malformed downstream results") {
    Router r;stream::Session s(r,&r);open(s);
    auto bad=request(s,3,1,{'a'});bad[10]=2;
    CHECK(s.execute(bad)[0]==2);CHECK(r.calls.size()==1);
    r.malformed=true;
    CHECK(s.execute(request(s,2,2,{},1))[0]==7);
    r.malformed=false;
    CHECK(s.execute(request(s,2,3,{},1))[0]==2);CHECK(r.calls.size()==2);
    CHECK(s.execute(request(s,4,4))[0]==0);
}
TEST_CASE("small stream capability token changes across endpoint reset") {
    Router r; AtariSioNioFrontend f(r);
    auto query=std::array<std::uint8_t,5>{0x7f,'s',0,0,0};
    query[4]=AtariSioNioFrontend::checksum(query.data(),4);
    auto before=f.begin(query);REQUIRE(before.accepted);CHECK(before.bytes.size()==13);
    f.reset_host(); auto after=f.begin(query);REQUIRE(after.accepted);
    CHECK(before.bytes!=after.bytes);
}
namespace {
std::array<std::uint8_t,5> frame(std::uint8_t op,unsigned n) {
    std::array<std::uint8_t,5> f{0x7f,op,static_cast<std::uint8_t>(n),static_cast<std::uint8_t>(n>>8),0};
    f[4]=AtariSioNioFrontend::checksum(f.data(),4);return f;
}
}
TEST_CASE("small stream SIO checksum rejection and exact submit replay do not duplicate work") {
    Router r; AtariSioNioFrontend f(r);
    auto query=f.begin(frame('s',0));REQUIRE(query.accepted);
    std::vector<std::uint8_t> req{1,1};
    req.insert(req.end(),query.bytes.begin()+4,query.bytes.begin()+8);
    req.insert(req.end(),{0,0,0,0,9,'t','c','p',':','/','/','h',':','9'});
    auto data=req;data.push_back(AtariSioNioFrontend::checksum(req.data(),req.size()));
    REQUIRE(f.begin(frame('S',req.size())).accepted);
    auto corrupt=data;corrupt.back()^=1;
    CHECK_FALSE(f.complete_write(corrupt.data(),corrupt.size()).accepted);
    CHECK(r.calls.empty());
    REQUIRE(f.begin(frame('S',req.size())).accepted);
    REQUIRE(f.complete_write(data.data(),data.size()).accepted);
    CHECK(r.calls.size()==1);
    auto first=f.begin(frame('T',11));REQUIRE(first.accepted);
    CHECK(f.begin(frame('T',11)).bytes==first.bytes);
    CHECK_FALSE(f.begin(frame('T',10)).accepted);
    REQUIRE(f.begin(frame('S',req.size())).accepted);
    REQUIRE(f.complete_write(data.data(),data.size()).accepted);
    CHECK(f.begin(frame('T',11)).bytes==first.bytes);
    CHECK(r.calls.size()==1);
    REQUIRE(f.begin(frame('S',req.size())).accepted);
    f.abort_write();
    CHECK(f.begin(frame('T',11)).bytes==first.bytes);
    f.reset_host();
    CHECK_FALSE(f.begin(frame('T',11)).accepted);
}
