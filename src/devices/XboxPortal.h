#pragma once
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <nsysuhs/uhs.h>

#define XBOX_PORTAL_VID    0x1430
#define XBOX_PORTAL_PID    0x1F17
#define PORTAL_PACKET_SIZE 64
#define XBOX_PREFIX_SIZE   2
#define UHS_BUFFER_ALIGN   64

class XboxPortalDevice {
public:
    XboxPortalDevice();
    ~XboxPortalDevice();

    bool StartPassthrough();
    void StopPassthrough();
    bool Read(uint8_t *buffer, uint32_t bufferLength);
    bool Write(uint8_t *buffer, uint32_t bufferLength);

private:
    void OnDeviceAttached(UhsInterfaceProfile *profile);
    bool SendCommand(uint8_t *buffer, uint32_t length);
    void ReadThread();

    UhsHandle m_uhsHandle;
    UhsConfig m_uhsConfig;
    uint32_t  m_ifHandle;
    uint8_t   m_inEpNum;
    uint32_t  m_inEpMask;
    uint8_t   m_outEpNum;
    bool      m_hasOutEp;

    std::thread                      m_readThread;
    std::mutex                       m_queueMutex;
    std::queue<std::vector<uint8_t>> m_packetQueue;
    bool m_stopThread;
    bool m_deviceReady;
};

bool XboxPortalIsConnected();
