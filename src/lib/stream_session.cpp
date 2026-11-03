#include "fujinet/io/stream/session.h"
#include <atomic>
#include <string>
#include <algorithm>
#include <limits>
namespace fujinet::io::stream {
namespace {
std::atomic<std::uint32_t> tokens{1};
unsigned get16(const std::vector<std::uint8_t>& p, unsigned at) { return p[at] | (p[at+1] << 8); }
std::uint32_t get32(const std::vector<std::uint8_t>& p, unsigned at) {
    return std::uint32_t(p[at]) | (std::uint32_t(p[at+1])<<8) |
        (std::uint32_t(p[at+2])<<16) | (std::uint32_t(p[at+3])<<24);
}
void put16(std::vector<std::uint8_t>& p, unsigned n) { p.push_back(n); p.push_back(n>>8); }
void put32(std::vector<std::uint8_t>& p, std::uint32_t n) {
    for (unsigned i=0;i<4;++i) p.push_back(n>>(8*i));
}
}
Session::Session(IRequestHandler& router, const void* owner)
    : _router(router), _owner(owner), _token(tokens.fetch_add(1)) {}
Session::~Session() { if (_open) call(0xfd, 4, {1, std::uint8_t(_handle), std::uint8_t(_handle>>8)}); }
IOResponse Session::call(std::uint8_t device, std::uint8_t command, std::vector<std::uint8_t> payload) {
    IORequest req; req.deviceId=device; req.command=command; req.clientOwner=_owner;
    req.payload=std::move(payload); return _router.handleRequest(req);
}
std::vector<std::uint8_t> Session::query() const {
    std::vector<std::uint8_t> p{1,max_data}; put32(p,_token); put32(p,_next); return p;
}
std::vector<std::uint8_t> Session::execute(const std::vector<std::uint8_t>& req) {
    std::vector<std::uint8_t> result(header,0);
    result[0]=static_cast<std::uint8_t>(StatusCode::InvalidRequest);
    if(req.size()<header) return result;
    std::copy(req.begin()+2, req.begin()+10, result.begin()+3);
    if(req[0]!=1 || get32(req,2)!=_token || !_token) return result;
    const auto seq=get32(req,6);
    const auto decision=_replay.inspect(seq,req.data(),req.size());
    if(decision==decltype(_replay)::Decision::Replay)
        return {_replay.result(),_replay.result()+_replay.result_size()};
    if(decision!=decltype(_replay)::Decision::Execute || _exhausted) return result;
    auto body=perform(req);
    result[0]=body[0]; result[1]=body[1]; result[2]=body[2];
    result.insert(result.end(),body.begin()+3,body.end());
    _replay.commit(seq,req.data(),req.size(),result.data(),result.size());
    _exhausted=seq==std::numeric_limits<std::uint32_t>::max();
    if(!_exhausted) _next=seq+1;
    return result;
}
std::vector<std::uint8_t> Session::perform(const std::vector<std::uint8_t>& req) {
    const auto failure=[](StatusCode code){return std::vector<std::uint8_t>{static_cast<std::uint8_t>(code),0,0};};
    const unsigned op=req[1], n=req[10];
    if(op!=5 && n>max_data) return failure(StatusCode::InvalidRequest);
    if((op==1 || op==3) ? req.size()!=header+n : req.size()!=header)
        return failure(StatusCode::InvalidRequest);
    if(op==1) {
        if(n<7 || !std::equal(req.begin()+header,req.begin()+header+6,"tcp://"))
            return failure(StatusCode::Unsupported);
        if(std::find(req.begin()+header,req.end(),0)!=req.end()) return failure(StatusCode::InvalidRequest);
        if(_open) {
            auto closed=call(0xfd,4,{1,std::uint8_t(_handle),std::uint8_t(_handle>>8)});
            if(closed.status!=StatusCode::Ok) return failure(closed.status);
            _open=false;
        }
        std::vector<std::uint8_t> p{1,0,0x20}; put16(p,n);
        p.insert(p.end(),req.begin()+header,req.end()); p.resize(p.size()+8,0);
        const auto r=call(0xfd,1,std::move(p));
        if(r.status!=StatusCode::Ok) return failure(r.status);
        if(r.payload.size()<7 || r.payload[0]!=1 || (r.payload[6]&7)!=7) {
            _poisoned=true; return failure(StatusCode::InternalError);
        }
        _handle=get16(r.payload,4); _open=true; _read=_write=0; _poisoned=false;
        return {0,0,0};
    }
    // Cold-path legacy lobby key lookup, without linking general FileDevice packets on the Atari.
    if(op==5) {
        const char* hex="0123456789abcdef";
        std::string uri="persist:///FujiNet/00010100.key";
        uri[25]=hex[n>>4]; uri[26]=hex[n&15];
        std::vector<std::uint8_t> p{1}; put16(p,uri.size()); p.insert(p.end(),uri.begin(),uri.end());
        put32(p,0); put16(p,64);
        const auto r=call(0xfe,3,std::move(p));
        if(r.status!=StatusCode::Ok) return failure(r.status);
        if(r.payload.size()<10 || r.payload[0]!=1 || get32(r.payload,4)!=0 ||
           get16(r.payload,8)>64 || r.payload.size()<10+get16(r.payload,8))
            return failure(StatusCode::InternalError);
        const unsigned count=get16(r.payload,8);
        std::vector<std::uint8_t> out{0,0,static_cast<std::uint8_t>(count)};
        out.insert(out.end(),r.payload.begin()+10,r.payload.begin()+10+count); return out;
    }
    if(op==4) {
        if(n) return failure(StatusCode::InvalidRequest);
        if(!_open) return {0,0,0};
        const auto r=call(0xfd,4,{1,std::uint8_t(_handle),std::uint8_t(_handle>>8)});
        if(r.status==StatusCode::Ok) _open=false;
        return failure(r.status);
    }
    if(!_open || _poisoned) return failure(StatusCode::InvalidRequest);
    if((op!=2 && op!=3) || !n) return failure(StatusCode::InvalidRequest);
    auto& offset=op==2?_read:_write;
    if(offset>std::numeric_limits<std::uint32_t>::max()-n) return failure(StatusCode::InvalidRequest);
    std::vector<std::uint8_t> p{1}; put16(p,_handle); put32(p,offset); put16(p,n);
    if(op==3) p.insert(p.end(),req.begin()+header,req.end());
    const auto r=call(0xfd,op,std::move(p));
    if(r.status!=StatusCode::Ok) return failure(r.status);
    if(r.payload.size()<12 || r.payload[0]!=1 || get16(r.payload,4)!=_handle ||
       get32(r.payload,6)!=offset || get16(r.payload,10)>n ||
       (op==2 && r.payload.size()<12+get16(r.payload,10))) {
        _poisoned=true; return failure(StatusCode::InternalError);
    }
    const unsigned count=get16(r.payload,10); offset+=count;
    std::vector<std::uint8_t> out{0,r.payload[1],static_cast<std::uint8_t>(count)};
    if(op==2) out.insert(out.end(),r.payload.begin()+12,r.payload.begin()+12+count);
    return out;
}
}
