// Link-time libusb replacement. This executable never links real libusb.
#include "mock_usb.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <array>
namespace mock_usb {
unsigned eventCalls, positiveWaits;
long lastWaitMicros;
unsigned devices, claims, releases, inLength, outSubmits, freesPending;
int openError, claimError, config, kernel, extendedError, oldSize, alternate;
unsigned vid, pid, endpointType, inAddress, outAddress, mps, interfaceClass;
bool present, inCallback, holdCancellation, lateSuccess;
std::vector<unsigned char> info, written;
std::deque<Result> input, output;
struct Pending { libusb_transfer* transfer; bool cancel; };
std::vector<Pending> transfers;
std::array<unsigned,8> identities{};
unsigned identity;
void reset() {
    assert(transfers.empty()); eventCalls = positiveWaits = 0; lastWaitMicros = 0; devices = 1; claims = releases = inLength = outSubmits = freesPending = 0;
    openError = claimError = kernel = alternate = 0; config = 1; extendedError = LIBUSB_ERROR_PIPE; oldSize = 6;
    vid = 0x082d; pid = 0x0100; endpointType = 2; inAddress = 0x82; outAddress = 2; mps = 64; interfaceClass = 255;
    present = true; inCallback = holdCancellation = lateSuccess = false; identity = 0;
    info = {2,0,0,1,0,2}; written.clear(); input.clear(); output.clear();
}
void replace_device() { ++identity; assert(identity + 1 < identities.size()); }
libusb_device* device(unsigned i = 0) { return reinterpret_cast<libusb_device*>(&identities[identity + i]); }
unsigned pending() { return transfers.size(); }
}
using namespace mock_usb;
extern "C" {
int LIBUSB_CALL libusb_init(libusb_context** c) { *c = reinterpret_cast<libusb_context*>(1); return 0; }
void LIBUSB_CALL libusb_exit(libusb_context*) { assert(transfers.empty()); }
ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context*, libusb_device*** list) {
    unsigned n = present ? devices : 0;
    *list = new libusb_device*[n+1]{}; for(unsigned i=0;i<n;++i) (*list)[i] = device(i); return n;
}
void LIBUSB_CALL libusb_free_device_list(libusb_device** l, int) { delete[] l; }
libusb_device* LIBUSB_CALL libusb_ref_device(libusb_device* d) { return d; }
void LIBUSB_CALL libusb_unref_device(libusb_device*) {}
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device*, libusb_device_descriptor* d) {
    *d = {}; d->idVendor = vid; d->idProduct = pid; return 0;
}
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device*) { return 58 + identity; }
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device*) { return 1; }
int LIBUSB_CALL libusb_get_port_numbers(libusb_device* d, uint8_t* p, int size) {
    assert(size>=2); p[0] = 9; p[1] = d == device() ? 4 : 5; return 2;
}
int LIBUSB_CALL libusb_open(libusb_device* d, libusb_device_handle** h) {
    if(openError) return openError; *h = reinterpret_cast<libusb_device_handle*>(d); return 0;
}
libusb_device* LIBUSB_CALL libusb_get_device(libusb_device_handle* h) { return reinterpret_cast<libusb_device*>(h); }
void LIBUSB_CALL libusb_close(libusb_device_handle*) { assert(transfers.empty()); }
int LIBUSB_CALL libusb_get_configuration(libusb_device_handle*, int* c) { *c = config; return 0; }
int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device*, libusb_config_descriptor** d) {
    static libusb_endpoint_descriptor eps[2];
    static libusb_interface_descriptor alt;
    static libusb_interface iface;
    eps[0] = {}; eps[1] = {};
    eps[0].bEndpointAddress = inAddress; eps[1].bEndpointAddress = outAddress;
    for(auto& ep:eps) { ep.bmAttributes = endpointType; ep.wMaxPacketSize = mps; }
    alt = {}; alt.bNumEndpoints = 2; alt.bInterfaceClass = interfaceClass; alt.endpoint = eps;
    iface = {}; iface.num_altsetting = 1; iface.altsetting = &alt;
    *d = new libusb_config_descriptor{}; (*d)->bConfigurationValue = config;
    (*d)->bNumInterfaces = 1; (*d)->interface = &iface; return 0;
}
void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor* d) { delete d; }
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle*, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
    unsigned char* data, uint16_t length, unsigned timeout) {
    assert(timeout && timeout <= 250); assert(value == 0);
    if(req == 4) { assert(type==0xc2 && index==0 && length==20); return extendedError; }
    if(req == 3) { assert(type==0xc2 && length==6); std::copy_n(info.data(), std::min<std::size_t>(6, info.size()),data); return oldSize; }
    assert(req == 10 && type == 0x81 && length == 1); data[0] = alternate; return 1;
}
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle*, int) { return kernel; }
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle*, int i) { assert(i==0); if(claimError) return claimError; ++claims; return 0; }
int LIBUSB_CALL libusb_release_interface(libusb_device_handle*, int) { assert(transfers.empty()); ++releases; return 0; }
libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int iso) { assert(iso==0); return static_cast<libusb_transfer*>(std::calloc(1,sizeof(libusb_transfer))); }
void LIBUSB_CALL libusb_free_transfer(libusb_transfer* t) {
    for(auto& p:transfers) if(p.transfer==t) ++freesPending;
    assert(!freesPending); std::free(t);
}
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer* t) {
    for(auto& p:transfers) assert(p.transfer!=t);
    assert(t->timeout==250 && t->type==LIBUSB_TRANSFER_TYPE_BULK);
    if(t->endpoint & 0x80) { inLength = t->length; assert(inLength == mps); }
    else ++outSubmits;
    transfers.push_back({t,false}); return 0;
}
int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer* t) {
    for(auto& p:transfers) if(p.transfer==t) { p.cancel=true; return 0; }
    return LIBUSB_ERROR_NOT_FOUND;
}
int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context*, timeval* tv, int*) {
    assert(tv && tv->tv_sec==0 && tv->tv_usec<=10000);
    ++eventCalls; lastWaitMicros = tv->tv_usec;
    if (tv->tv_usec) ++positiveWaits;
    // At most one callback per pump; ownership remains with libusb until callback.
    for(std::size_t i=0;i<transfers.size();++i) {
        auto pending = transfers[i]; auto* t = pending.transfer;
        bool in = t->endpoint & 0x80;
        if(pending.cancel && holdCancellation) continue;
        auto& q = in ? input : output;
        if(!pending.cancel && in && q.empty()) continue;
        Result r{LIBUSB_TRANSFER_COMPLETED,{},-1};
        if(pending.cancel && !(in && lateSuccess && !q.empty())) r.status=LIBUSB_TRANSFER_CANCELLED;
        else if(!q.empty()) { r=std::move(q.front()); q.pop_front(); }
        t->status=r.status;
        t->actual_length=r.actual>=0 ? r.actual : (in ? std::min<int>(t->length,r.data.size()) : t->length);
        if(in) {
            const auto n = std::min<std::size_t>(t->length,r.data.size());
            std::copy_n(r.data.begin(),n,t->buffer);
            if(n<r.data.size()) { r.data.erase(r.data.begin(),r.data.begin()+n); q.push_front(std::move(r)); }
        } else if(t->actual_length>0 && t->actual_length<=t->length)
            written.insert(written.end(),t->buffer,t->buffer+t->actual_length);
        transfers.erase(transfers.begin()+i);
        inCallback = true; t->callback(t); inCallback = false;
        break;
    }
    return 0;
}
const char* LIBUSB_CALL libusb_error_name(int code) {
    switch(code) {
    case LIBUSB_ERROR_ACCESS: return "LIBUSB_ERROR_ACCESS";
    case LIBUSB_ERROR_BUSY: return "LIBUSB_ERROR_BUSY";
    case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE";
    default: return "LIBUSB_ERROR_OTHER";
    }
}
}
