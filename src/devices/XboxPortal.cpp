#include "XboxPortal.h"
#include <cstring>
#include <malloc.h>
#include <chrono>
#include <thread>
#include <algorithm>

XboxPortalDevice::XboxPortalDevice()
    : m_ifHandle(0), m_inEpNum(0),
      m_inEpMask(0), m_outEpNum(0), m_hasOutEp(false),
      m_stopThread(false), m_deviceReady(false) {
    memset(&m_uhsHandle, 0, sizeof(m_uhsHandle));
    memset(&m_uhsConfig, 0, sizeof(m_uhsConfig));
}

XboxPortalDevice::~XboxPortalDevice() {
    StopPassthrough();
}

void XboxPortalDevice::OnDeviceAttached(UhsInterfaceProfile *profile) {
    m_ifHandle = profile->if_handle;

    for (int i = 0; i < 16; i++) {
        auto &ep = profile->in_endpoints[i];
        if (ep.bLength == 0) break;
        if (ep.bmAttributes == 3) {
            m_inEpNum  = ep.bEndpointAddress & 0x0F;
            m_inEpMask = (uint8_t)(1u << m_inEpNum);
        }
    }
    m_hasOutEp    = false;
    m_deviceReady = true;
}

bool XboxPortalDevice::StartPassthrough() {
    UHSStatus status = UhsClientOpen(&m_uhsHandle, &m_uhsConfig);
    if (status != UHS_STATUS_OK) return false;

    UhsInterfaceFilter filter{};
    filter.match_params = MATCH_DEV_VID | MATCH_DEV_PID;
    filter.vid          = XBOX_PORTAL_VID;
    filter.pid          = XBOX_PORTAL_PID;

    UhsInterfaceProfile profile{};
    status = UhsQueryInterfaces(&m_uhsHandle, &filter, &profile, 1);
    if (status != UHS_STATUS_OK) {
        UhsClientClose(&m_uhsHandle);
        return false;
    }

    OnDeviceAttached(&profile);

    m_stopThread = false;
    m_readThread = std::thread(&XboxPortalDevice::ReadThread, this);
    return true;
}

void XboxPortalDevice::StopPassthrough() {
    m_stopThread = true;
    if (m_readThread.joinable()) m_readThread.join();
    if (m_deviceReady) {
        UhsClientClose(&m_uhsHandle);
        m_deviceReady = false;
    }
}

void XboxPortalDevice::ReadThread() {
    auto *buf = static_cast<uint8_t *>(
        memalign(UHS_BUFFER_ALIGN, PORTAL_PACKET_SIZE + UHS_BUFFER_ALIGN));
    if (!buf) return;

    while (!m_stopThread) {
        if (!m_deviceReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // UhsSubmitBulkRequest(handle, ifHandle, epMask, direction, buffer, length, timeout)
        // direction: 1 = IN (read from device)
        UHSStatus st = UhsSubmitBulkRequest(
            &m_uhsHandle,
            m_ifHandle,
            m_inEpMask,
            1,                  // direction IN
            buf,
            PORTAL_PACKET_SIZE,
            100);

        if (st == UHS_STATUS_OK) {
            uint8_t  *payload = buf;
            uint32_t  length  = PORTAL_PACKET_SIZE;

            if (length >= 2 &&
                payload[0] == 0x0B &&
                payload[1] == 0x14) {
                payload += XBOX_PREFIX_SIZE;
                length  -= XBOX_PREFIX_SIZE;
            }

            std::vector<uint8_t> packet(payload, payload + length);
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_packetQueue.push(std::move(packet));
        }
    }
    free(buf);
}

bool XboxPortalDevice::SendCommand(uint8_t *buffer, uint32_t length) {
    if (!m_deviceReady) return false;
    // direction: 0 = OUT (write to device)
    return UhsSubmitBulkRequest(
               &m_uhsHandle,
               m_ifHandle,
               (uint8_t)(1u << m_outEpNum),
               0,               // direction OUT
               buffer,
               (int32_t)length,
               200)
           == UHS_STATUS_OK;
}

bool XboxPortalDevice::Read(uint8_t *buffer, uint32_t bufferLength) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_packetQueue.empty()) return false;
    auto &pkt = m_packetQueue.front();
    uint32_t len = std::min((uint32_t)pkt.size(), bufferLength);
    memcpy(buffer, pkt.data(), len);
    m_packetQueue.pop();
    return true;
}

bool XboxPortalDevice::Write(uint8_t *buffer, uint32_t bufferLength) {
    return SendCommand(buffer, bufferLength);
}

bool XboxPortalIsConnected() {
    UhsHandle h{};
    UhsConfig c{};
    if (UhsClientOpen(&h, &c) != UHS_STATUS_OK) return false;

    UhsInterfaceFilter filter{};
    filter.match_params = MATCH_DEV_VID | MATCH_DEV_PID;
    filter.vid          = XBOX_PORTAL_VID;
    filter.pid          = XBOX_PORTAL_PID;

    UhsInterfaceProfile profile{};
    UHSStatus st = UhsQueryInterfaces(&h, &filter, &profile, 1);
    UhsClientClose(&h);
    return st == UHS_STATUS_OK;
}
