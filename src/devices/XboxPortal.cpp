#include "XboxPortal.h"
#include <cstring>
#include <malloc.h>
#include <coreinit/cache.h>
#include <whb/log.h>

#define UHS_BUFFER_SIZE 0x1000

static XboxPortalDevice *s_instance = nullptr;

XboxPortalDevice::XboxPortalDevice()
    : m_uhsBuffer(nullptr), m_ifHandle(0), m_inEpNum(0),
      m_inEpMask(0), m_outEpNum(0), m_hasOutEp(false),
      m_stopThread(false), m_deviceReady(false) {
    s_instance = this;
    memset(&m_uhsHandle, 0, sizeof(m_uhsHandle));
    memset(&m_uhsConfig, 0, sizeof(m_uhsConfig));
    m_uhsBuffer = memalign(UHS_BUFFER_ALIGN, UHS_BUFFER_SIZE);
}

XboxPortalDevice::~XboxPortalDevice() {
    StopPassthrough();
    if (m_uhsBuffer) {
        free(m_uhsBuffer);
        m_uhsBuffer = nullptr;
    }
    s_instance = nullptr;
}

void XboxPortalDevice::ClassDriverCallback(void *context,
                                            UhsInterfaceProfile *profile) {
    if (!profile || !s_instance) return;
    if (profile->desc.idVendor  == XBOX_PORTAL_VID &&
        profile->desc.idProduct == XBOX_PORTAL_PID) {
        s_instance->OnDeviceAttached(profile);
    }
}

void XboxPortalDevice::OnDeviceAttached(UhsInterfaceProfile *profile) {
    m_ifHandle = profile->ifHandle;

    for (int i = 0; i < profile->numEndpoints; i++) {
        auto &ep = profile->endpoints[i];
        if ((ep.desc.bEndpointAddress & 0x80) &&
             ep.desc.bmAttributes == 3) {      // interrupt IN
            m_inEpNum  = ep.desc.bEndpointAddress & 0x0F;
            m_inEpMask = ep.epMask;
        } else if (!(ep.desc.bEndpointAddress & 0x80) &&
                    ep.desc.bmAttributes == 3) { // interrupt OUT
            m_outEpNum  = ep.desc.bEndpointAddress & 0x0F;
            m_hasOutEp  = true;
        }
    }
    m_deviceReady = true;
}

bool XboxPortalDevice::StartPassthrough() {
    m_uhsConfig.usbTimeout = 5000;

    UhsStatus status = UhsClientOpen(&m_uhsHandle, &m_uhsConfig);
    if (status != UHS_STATUS_OK) return false;

    UhsInterfaceFilter filter{};
    filter.match_params  = UHS_MATCH_DEV_VID | UHS_MATCH_DEV_PID;
    filter.idVendor      = XBOX_PORTAL_VID;
    filter.idProduct     = XBOX_PORTAL_PID;

    status = UhsClassDriverRegister(&m_uhsHandle, &filter,
                                    ClassDriverCallback, this);
    if (status != UHS_STATUS_OK) {
        UhsClientClose(&m_uhsHandle);
        return false;
    }

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
            OSSleepTicks(OSMillisecondsToTicks(10));
            continue;
        }

        uint32_t transferred = 0;
        UhsStatus st = UhsSubmitBulkMsg(
            &m_uhsHandle, m_ifHandle, m_inEpMask,
            buf, PORTAL_PACKET_SIZE, &transferred, 100);

        if (st == UHS_STATUS_OK && transferred > 0) {
            uint8_t  *payload = buf;
            uint32_t  length  = transferred;

            // Strip the 0x0B 0x14 Xbox prefix if present
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

    if (m_hasOutEp) {
        uint32_t transferred = 0;
        return UhsSubmitBulkMsg(&m_uhsHandle, m_ifHandle,
                                1u << m_outEpNum,
                                buffer, length, &transferred, 200)
               == UHS_STATUS_OK;
    }

    // Fall back to HID control transfer
    return UhsHidSetReport(&m_uhsHandle, m_ifHandle,
                           0x02, buffer[0], buffer, length)
           == UHS_STATUS_OK;
}

// ── Device interface methods ──────────────────────────────────────────────────

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

bool XboxPortalDevice::GetDescriptor(uint8_t, uint8_t, uint16_t,
                                      uint8_t *, uint32_t) { return false; }
bool XboxPortalDevice::SetDescriptor(uint8_t, uint8_t, uint16_t,
                                      uint8_t *, uint32_t) { return false; }
bool XboxPortalDevice::GetReport(uint8_t *, uint32_t)       { return false; }
bool XboxPortalDevice::SetReport(uint8_t *, uint32_t)       { return false; }
bool XboxPortalDevice::GetIdle(uint8_t, uint8_t, uint8_t *) { return false; }
bool XboxPortalDevice::SetIdle(uint8_t, uint8_t, uint8_t)   { return false; }
bool XboxPortalDevice::GetProtocol(uint8_t, uint8_t *)      { return false; }
bool XboxPortalDevice::SetProtocol(uint8_t, uint8_t)        { return false; }

// ── Static presence check ────────────────────────────────────────────────────

bool XboxPortalIsConnected() {
    UhsHandle  h{};
    UhsConfig  c{};
    c.usbTimeout = 1000;
    if (UhsClientOpen(&h, &c) != UHS_STATUS_OK) return false;

    UhsInterfaceProfile profiles[4]{};
    uint32_t count = 0;
    UhsQueryInterfaces(&h, XBOX_PORTAL_VID, XBOX_PORTAL_PID,
                       profiles, 4, &count);
    UhsClientClose(&h);
    return count > 0;
}
