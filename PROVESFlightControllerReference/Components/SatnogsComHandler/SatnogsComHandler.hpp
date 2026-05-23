// ======================================================================
// \title  SatnogsComHandler.hpp
// \brief  Receives and parses ASCII log lines from a SatNOGS-COMMS module
//         over UART. Protocol: every line is "LOG: <content>\n".
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
    bool parseTxUhf(const char* content, U32& outLen, U32& outIface);
    void sendToUart(const char* str);

    static constexpr U32 LINE_BUFFER_SIZE = 256;
    static constexpr U32 LOG_PREFIX_LEN = 5;  // "LOG: "

    char m_lineBuf[LINE_BUFFER_SIZE];
    U32  m_lineBufSize;
    U32  m_linesReceived;
    U32  m_txUhfCount;
    bool m_booted;
};

}  // namespace Components

#endif
