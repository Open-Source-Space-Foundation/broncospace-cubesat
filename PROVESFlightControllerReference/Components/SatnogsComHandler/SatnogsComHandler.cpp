// ======================================================================
// \title  SatnogsComHandler.cpp
// \brief  Parses ASCII "LOG: <content>\n" lines from a SatNOGS-COMMS module
//         and appends every line to /satnogs.log on the SD card for later
//         downlink via FileHandling.fileDownlink.
//
// Protocol (from ICD):
//   - Every line has the form: LOG: <content>\n
//   - First boot line (>64 bytes) may arrive split across UART reads
//   - Section headers: POWER, RADIO, HEALTH, CONFIG, FPGA, TIME, BOOTLOADER INFO
//   - TX notifications: TX UHF: len=N iface=N
// ======================================================================

#include "PROVESFlightControllerReference/Components/SatnogsComHandler/SatnogsComHandler.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace Components {

SatnogsComHandler::SatnogsComHandler(const char* compName)
    : SatnogsComHandlerComponentBase(compName),
      m_lineBufSize(0),
      m_linesReceived(0),
      m_txUhfCount(0),
      m_booted(false),
      m_fileOpen(false) {
    memset(m_lineBuf, 0, sizeof(m_lineBuf));
}

// ----------------------------------------------------------------------
// openLogFile — opens /satnogs.log in append mode (lazy, on first data)
// ----------------------------------------------------------------------

void SatnogsComHandler::openLogFile() {
    Os::File::Status stat = m_logFile.open(LOG_FILE_PATH, Os::File::OPEN_APPEND);
    if (stat != Os::File::OP_OK) {
        this->log_WARNING_HI_LogFileError(OP_OPEN, static_cast<I32>(stat));
        return;
    }
    m_fileOpen = true;
    Fw::LogStringArg path(LOG_FILE_PATH);
    this->log_ACTIVITY_HI_LogFileOpened(path);
}

// ----------------------------------------------------------------------
// writeToLog — writes "LOG: <content>\n" to the open file
// ----------------------------------------------------------------------

void SatnogsComHandler::writeToLog(const char* line, U32 len) {
    if (!m_fileOpen) {
        return;
    }

    // Write the full original line including "LOG: " prefix and newline
    FwSizeType written = static_cast<FwSizeType>(len);
    Os::File::Status stat = m_logFile.write(reinterpret_cast<const U8*>(line), written);
    if (stat != Os::File::OP_OK) {
        this->log_WARNING_HI_LogFileError(OP_WRITE, static_cast<I32>(stat));
        return;
    }

    // Write newline
    const U8 newline = '\n';
    FwSizeType nlWritten = 1;
    m_logFile.write(&newline, nlWritten);
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

    // Open file lazily on first data received
    if (!m_fileOpen) {
        openLogFile();
    }

    const U8* data = buffer.getData();
    U32 size = static_cast<U32>(buffer.getSize());

    for (U32 i = 0; i < size; i++) {
        char c = static_cast<char>(data[i]);

        if (c == '\n') {
            // Strip trailing \r
            if (m_lineBufSize > 0 && m_lineBuf[m_lineBufSize - 1] == '\r') {
                m_lineBufSize--;
            }
            m_lineBuf[m_lineBufSize] = '\0';

            if (m_lineBufSize > 0) {
                // Write raw line to SD card before parsing
                writeToLog(m_lineBuf, m_lineBufSize);
                processLine(m_lineBuf, m_lineBufSize);
            }
            m_lineBufSize = 0;
        } else if (m_lineBufSize < LINE_BUFFER_SIZE - 1) {
            m_lineBuf[m_lineBufSize++] = c;
        } else {
            // ICD: first boot line (~67 bytes) can be discarded if it overflows
            this->log_WARNING_LO_LineTruncated();
            m_lineBufSize = 0;
        }
    }

    this->bufferReturn_out(0, buffer);
}

// ----------------------------------------------------------------------
// processLine — fires F-Prime events for key line types
// ----------------------------------------------------------------------

void SatnogsComHandler::processLine(const char* line, U32 len) {
    // Every valid SatNOGS line starts with "LOG: "
    if (len < LOG_PREFIX_LEN || strncmp(line, "LOG: ", LOG_PREFIX_LEN) != 0) {
        return;
    }

    const char* content = line + LOG_PREFIX_LEN;

    m_linesReceived++;
    this->tlmWrite_LinesReceived(m_linesReceived);

    // TX UHF notification: "TX UHF: len=N iface=N"
    if (strncmp(content, "TX UHF:", 7) == 0) {
        U32 txLen = 0, iface = 0;
        if (parseTxUhf(content, txLen, iface)) {
            m_txUhfCount++;
            this->tlmWrite_TxUhfCount(m_txUhfCount);
            this->log_ACTIVITY_LO_TxDetected(txLen, iface);
        }
        return;
    }

    // Section headers
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

    // Boot message
    if (!m_booted && strncmp(content, "SatNOGS-COMMS FW:", 17) == 0) {
        m_booted = true;
        Fw::LogStringArg msg(content);
        this->log_ACTIVITY_HI_BootMessage(msg);
        return;
    }

    // Thread ready
    if (strcmp(content, "All threads started successfully!") == 0) {
        Fw::LogStringArg msg(content);
        this->log_ACTIVITY_LO_SatnogsLog(msg);
        return;
    }

    // Everything else (hex data lines, config lines) is already written to
    // the SD card above — no need to also fire an event for each one.
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
// schedIn — 1Hz tick; nothing to do currently
// ----------------------------------------------------------------------

void SatnogsComHandler::schedIn_handler(FwIndexType portNum, U32 context) {
    (void)portNum;
    (void)context;
}

// ----------------------------------------------------------------------
// SEND_COMMAND — forwards arbitrary string to SatNOGS over UART
// ----------------------------------------------------------------------

void SatnogsComHandler::SEND_COMMAND_cmdHandler(FwOpcodeType opCode, U32 cmdSeq,
                                                  const Fw::CmdStringArg& cmd) {
    sendToUart(cmd.toChar());
    Fw::LogStringArg logCmd(cmd);
    this->log_ACTIVITY_LO_SatnogsLog(logCmd);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

// ----------------------------------------------------------------------
// CLOSE_LOG — safely closes the log file before requesting file downlink
// ----------------------------------------------------------------------

void SatnogsComHandler::CLOSE_LOG_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    if (m_fileOpen) {
        m_logFile.close();
        m_fileOpen = false;
        this->log_ACTIVITY_HI_LogFileClosed();
    }
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void SatnogsComHandler::sendToUart(const char* str) {
    Fw::Buffer buf(reinterpret_cast<U8*>(const_cast<char*>(str)),
                   static_cast<U32>(strlen(str)));
    Drv::ByteStreamStatus status = this->commandOut_out(0, buf);
    if (status != Drv::ByteStreamStatus::OP_OK) {
        this->log_WARNING_HI_SatnogsError(2);
    }
}

}  // namespace Components
