// ======================================================================
// \title  SatnogsComHandler.hpp
// \brief  Receives and parses ASCII log lines from a SatNOGS-COMMS module
//         over UART. Protocol: every line is "LOG: <content>\n".
//         All received data lines are emitted as F-Prime events for
//         downlink during a pass. No filesystem access.
// ======================================================================

#ifndef Components_SatnogsComHandler_HPP
#define Components_SatnogsComHandler_HPP

#include "PROVESFlightControllerReference/Components/SatnogsComHandler/SatnogsComHandlerComponentAc.hpp"

namespace Components {

class SatnogsComHandler final : public SatnogsComHandlerComponentBase {
  public:
    SatnogsComHandler(const char* compName);
    ~SatnogsComHandler() = default;

  private:
    void dataIn_handler(FwIndexType portNum, Fw::Buffer& buffer,
                        const Drv::ByteStreamStatus& status) override;
    void schedIn_handler(FwIndexType portNum, U32 context) override;
    void SEND_COMMAND_cmdHandler(FwOpcodeType opCode, U32 cmdSeq,
                                 const Fw::CmdStringArg& cmd) override;

    void processLine(const char* line, U32 len);
    void processContent(const char* content);
    bool parseTxUhf(const char* content, U32& outLen, U32& outIface);
    bool parseConfigEcho(const char* content);
    bool appendSectionHex(const char* content);
    void flushSection();
    void decodeHealth(const U8* msg, U32 len);
    void sendToUart(const char* str);

    static constexpr U32 LINE_BUFFER_SIZE = 256;
    static constexpr U32 LOG_PREFIX_LEN = 5;  // "LOG: "

    // Section accumulation: hex lines are decoded and gathered here until the
    // section ends, then written to the section's telemetry channel.
    enum Section : U8 {
        SECTION_NONE = 0,
        SECTION_POWER = 1,
        SECTION_RADIO = 2,
        SECTION_HEALTH = 3,
        SECTION_CONFIG = 4,
        SECTION_FPGA = 5,
        SECTION_TIME = 6,
        SECTION_BOOTLOADER = 7,
    };

    static constexpr U32 SECTION_BUFFER_SIZE = 128;
    static constexpr U32 SECTION_IDLE_FLUSH_TICKS = 2;  // flush open section after 2s of silence

    char m_lineBuf[LINE_BUFFER_SIZE];
    U32  m_lineBufSize;
    U32  m_linesReceived;
    U32  m_txUhfCount;
    bool m_booted;

    Section m_section;
    U8      m_sectionBuf[SECTION_BUFFER_SIZE];
    U32     m_sectionLen;
    U32     m_sectionIdleTicks;
};

}  // namespace Components

#endif
