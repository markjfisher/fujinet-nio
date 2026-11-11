#pragma once
#include <libusb.h>
#include <chrono>
#include <deque>
#include <string>
#include <vector>
namespace mock_usb {
struct Result { libusb_transfer_status status; std::vector<unsigned char> data; int actual{-1}; };
extern unsigned eventCalls, positiveWaits;
extern long lastWaitMicros;
extern unsigned opens, closes, references;
extern unsigned devices, claims, releases, inLength, outSubmits, freesPending, controlTransfers;
extern int openError, claimError, config, kernel, extendedError, oldSize, alternate;
extern unsigned vid, pid, endpointType, inAddress, outAddress, mps, interfaceClass;
extern bool holdOutput;
extern bool present, inCallback, holdCancellation, lateSuccess;
extern std::vector<unsigned char> info, written;
extern std::deque<Result> input, output;
std::chrono::steady_clock::time_point now();
void advance(std::chrono::milliseconds elapsed);
void reset();
void replace_device();
unsigned pending();
}
