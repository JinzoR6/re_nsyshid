#pragma once

#include "Device.h"
#include <nsysuhs/uhs.h>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#define XBOX_PORTAL_VID        0x1430
#define XBOX_PORTAL_PID        0x1F17
#define WII_PORTAL_PID         0x0150
#define PORTAL_PACKET_SIZE     32
#define XBOX_PREFIX_SIZE       2
#define UHS_BUFFER_ALIGN       0x40

class XboxPortalDevice : public Device {
public:
    XboxPortalDevice();
    ~XboxPortalDevice();

    bool GetDescriptor(uint8_t descType, uint8_t descIndex, uint16_t lang,
                       uint8_t *buffer, uint32_t bufferLength) override;
    bool SetDescriptor(uint8_t descType, uint8_t descIndex, uint16_t lang,
                       uint8_t *buffer, uint32_t bufferLength) override;
    bool GetReport(uint8_t *buffer, uint32_t bufferLength) override;
    bool SetReport(uint8_t *buffer, uint32_t bufferLength) override;
    bool GetIdle(uint8_t ifIndex, uint8_t reportId, uint8_t *duration) override;
    bool SetIdle(uint8_t ifIndex, uint8_t reportId, uint8_t duration) override;
    bool GetProtocol(uint8_t ifIndex, uint8_t *protocol) override;
    bool SetProtocol(uint8_t ifIndex, uint8_t protocol) override;
    bool Read(uint8_t *buffer, uint32_t bufferLength) override;
    bool Write(uint8_t *buffer, uint32_t bufferLength) override;

    bool StartPassthrough();
    void StopPassthrough();

private:
    static void ClassDriverCallback(void *context, UhsInterfaceProfile *profile);
    void        OnDeviceAttached(UhsInterfaceProfile *profile);
    void        ReadThread();
    bool        SendCommand(uint8_t *buffer, uint32_t length);

    UhsHandle  m_uhsHandle;
    UhsConfig  m_uhsConfig;
    void      *m_uhsBuffer;

    uint32_t m_ifHandle;
    uint8_t  m_inEpNum;
    uint32_t m_inEpMask;
    uint8_t  m_outEpNum;
    bool     m_hasOutEp;

    std::thread        m_readThread;
    std::atomic<bool>  m_stopThread;
    std::atomic<bool>  m_deviceReady;

    std::mutex                        m_queueMutex;
    std::queue<std::vector<uint8_t>>  m_packetQueue;
};

// Check if Xbox portal is physically present (call before creating device)
bool XboxPortalIsConnected();
