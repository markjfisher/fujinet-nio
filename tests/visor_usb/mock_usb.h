#pragma once
#include <libusb.h>
#include <deque>
#include <string>
#include <vector>
namespace mock_usb {
struct Result { libusb_transfer_status status; std::vector<unsigned char> data; int actual{-1}; };
extern unsigned eventCalls, positiveWaits;
extern long lastWaitMicros;
extern unsigned devices, claims, releases, inLength, outSubmits, freesPending;
extern int openError, claimError, config, kernel, extendedError, oldSize, alternate;
extern unsigned vid, pid, endpointType, inAddress, outAddress, mps, interfaceClass;
extern bool present, inCallback, holdCancellation, lateSuccess;
extern std::vector<unsigned char> info, written;
extern std::deque<Result> input, output;
void reset();
void replace_device();
unsigned pending();
}
