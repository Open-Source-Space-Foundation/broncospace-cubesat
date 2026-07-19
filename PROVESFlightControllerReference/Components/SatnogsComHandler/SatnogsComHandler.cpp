// ======================================================================
// \title  SatnogsComHandler.cpp
// \brief  Parses ASCII "LOG: <content>\n" lines from a SatNOGS-COMMS module.
//
// Protocol (from ICD):
//   - Every line has the form: LOG: <content>\n
//   - First boot line (>64 bytes) may arrive split across UART reads; it is
//     discarded if it overflows the line buffer.
//   - Section headers: POWER, RADIO, HEALTH, CONFIG, FPGA, TIME, BOOTLOADER INFO
//     followed by hex-encoded binary telemetry lines.
//   - TX notifications: TX UHF: len=N iface=N
// ======================================================================

#include "PROVESFlightControllerReference/Components/SatnogsComHandler/SatnogsComHandler.hpp"

#include <cstring>
#include <cstdlib>

namespace Components {

SatnogsComHandler::SatnogsComHandler(const char* compName)
    : SatnogsComHandlerComponentBase(compName),
      m_lineBufSize(0),
      m_linesReceived(0),
      m_txUhfCount(0),
      m_booted(false) {
    memset(m_lineBuf, 0, sizeof(m_lineBuf));
}

// ----------------------------------------------------------------------
// dataIn — called by ZephyrUartDriver with a 64-byte chunk of UART RX data
// ----------------------------------------------------------------------

void SatnogsComHandler::dataIn_handler(FwIndexType portNum, Fw::Buffer& buffer,
                                        const Drv::ByteStreamStatus& status) {
    if (status != Drv::ByteStreamStatus::OP_OK) {
        this->log_WARNING_HI_SatnogsError(1);
        if (buffer.isValid()) {
            this->bufferReturn_out(0, buffer);
        }
        return;
    }

    if (!buffer.isValid()) {
        return;
    }

    const U8* data = buffer.getData();
    U32 size = static_cast<U32>(buffer.getSize());

    for (U32 i = 0; i < size; i++) {
        char c = static_cast<char>(data[i]);

        if (c == '\n') {
            // Strip trailing \r (Windows-style line endings)
            if (m_lineBufSize > 0 && m_lineBuf[m_lineBufSize - 1] == '\r') {
                m_lineBufSize--;
            }
            m_lineBuf[m_lineBufSize] = '\0';

            if (m_lineBufSize > 0) {
                processLine(m_lineBuf, m_lineBufSize);
            }
            m_lineBufSize = 0;
        } else if (m_lineBufSize < LINE_BUFFER_SIZE - 1) {
            m_lineBuf[m_lineBufSize++] = c;
        } else {
            // Line is longer than our buffer — ICD says first boot line (67-68
            // bytes) can be discarded. Log a warning and reset.
            this->log_WARNING_LO_LineTruncated();
            m_lineBufSize = 0;
        }
    }

    this->bufferReturn_out(0, buffer);
}

// ----------------------------------------------------------------------
// processLine — called with a complete null-terminated line (no \n)
// ----------------------------------------------------------------------

void SatnogsComHandler::processLine(const char* line, U32 len) {
    // Every valid SatNOGS line starts with "LOG: "
    if (len < LOG_PREFIX_LEN || strncmp(line, "LOG: ", LOG_PREFIX_LEN) != 0) {
        return;
    }

    const char* content = line + LOG_PREFIX_LEN;

    m_linesReceived++;
    this->tlmWrite_LinesReceived(m_linesReceived);

    // --- TX UHF notification: "TX UHF: len=N iface=N" ---
    if (strncmp(content, "TX UHF:", 7) == 0) {
        U32 txLen = 0, iface = 0;
        if (parseTxUhf(content, txLen, iface)) {
            m_txUhfCount++;
            this->tlmWrite_TxUhfCount(m_txUhfCount);
            this->log_ACTIVITY_LO_TxDetected(txLen, iface);
        }
        return;
    }

    // --- Section headers ---
    struct { const char* name; U8 id; } sections[] = {
        {"POWER",          1},
        {"RADIO",          2},
        {"HEALTH",         3},
        {"CONFIG",         4},
        {"FPGA",           5},
        {"TIME",           6},
        {"BOOTLOADER INFO",7},
    };
    for (auto& s : sections) {
        if (strcmp(content, s.name) == 0) {
            this->tlmWrite_LastSectionId(s.id);
            Fw::LogStringArg sectionArg(s.name);
            this->log_ACTIVITY_LO_SectionReceived(sectionArg);
            return;
        }
    }

    // --- Boot message: "SatNOGS-COMMS FW: ..." ---
    if (!m_booted && strncmp(content, "SatNOGS-COMMS FW:", 17) == 0) {
        m_booted = true;
        Fw::LogStringArg msg(content);
        this->log_ACTIVITY_HI_BootMessage(msg);
        return;
    }

    // --- Thread ready message ---
    if (strcmp(content, "All threads started successfully!") == 0) {
        Fw::LogStringArg msg(content);
        this->log_ACTIVITY_LO_SatnogsLog(msg);
        return;
    }

    // --- Hex data lines and everything else: silently consume.
    // Logging every hex line would flood downlink; the section header
    // already tells us which telemetry block arrived.
}

// ----------------------------------------------------------------------
// parseTxUhf — extracts len and iface from "TX UHF: len=N iface=N"
// ----------------------------------------------------------------------

bool SatnogsComHandler::parseTxUhf(const char* content, U32& outLen, U32& outIface) {
    const char* lenStr   = strstr(content, "len=");
    const char* ifaceStr = strstr(content, "iface=");
    if (!lenStr || !ifaceStr) {
        return false;
    }
    outLen   = static_cast<U32>(atoi(lenStr   + 4));
    outIface = static_cast<U32>(atoi(ifaceStr + 6));
    return true;
}

// ----------------------------------------------------------------------
// schedIn — 1Hz tick; nothing to do yet, placeholder for future health checks
// ----------------------------------------------------------------------

void SatnogsComHandler::schedIn_handler(FwIndexType portNum, U32 context) {
    (void)portNum;
    (void)context;
}

// ----------------------------------------------------------------------
// SEND_COMMAND — forwards an arbitrary string to the SatNOGS module over UART
// ----------------------------------------------------------------------

void SatnogsComHandler::SEND_COMMAND_cmdHandler(FwOpcodeType opCode, U32 cmdSeq,
                                                  const Fw::CmdStringArg& cmd) {
    sendToUart(cmd.toChar());
    Fw::LogStringArg logCmd(cmd);
    this->log_ACTIVITY_LO_SatnogsLog(logCmd);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void SatnogsComHandler::sendToUart(const char* str) {
    // ZephyrUartDriver send is synchronous, so passing a stack pointer is safe.
    Fw::Buffer buf(reinterpret_cast<U8*>(const_cast<char*>(str)),
                   static_cast<U32>(strlen(str)));
    Drv::ByteStreamStatus status = this->commandOut_out(0, buf);
    if (status != Drv::ByteStreamStatus::OP_OK) {
        this->log_WARNING_HI_SatnogsError(2);
    }
}

}  // namespace Components
