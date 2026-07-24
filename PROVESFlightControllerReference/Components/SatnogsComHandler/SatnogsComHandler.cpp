// ======================================================================
// \title  SatnogsComHandler.cpp
// \brief  Parses ASCII "LOG: <content>\n" lines from a SatNOGS-COMMS module.
//         Section payloads (hex lines after POWER/RADIO/HEALTH/CONFIG/FPGA/
//         TIME/BOOTLOADER INFO headers) are decoded to raw bytes and written
//         to per-section telemetry channels for on-demand downlink; UHF boot
//         configuration echoes are parsed into typed channels. No filesystem
//         access.
//
// Protocol (observed from hardware):
//   - Every record has the form: LOG: <content>\n
//   - Records are occasionally glued together with a missing newline, so
//     content is additionally split on embedded "LOG: " markers
//   - Section headers: POWER, RADIO, HEALTH, CONFIG, FPGA, TIME, BOOTLOADER INFO
//   - Section data: ASCII-hex lines encoding the radio's native message bytes
//     (msgId(2) + hdr(3) + len(1) + payload) — decoded on the ground per ICD
//   - TX notifications: TX UHF: len=N iface=N
//   - Boot config echoes: "UHF RX_FREQ = 437400000", "UHF TX_GAIN = 31.00", ...
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
      m_rebootCount(0),
      m_section(SECTION_NONE),
      m_sectionLen(0),
      m_sectionIdleTicks(0) {
    memset(m_lineBuf, 0, sizeof(m_lineBuf));
    memset(m_sectionBuf, 0, sizeof(m_sectionBuf));
}

// ----------------------------------------------------------------------
// dataIn — called by ZephyrUartDriver with a chunk of UART RX data
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
            // Strip trailing \r
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
            // ICD: first boot line (~67 bytes) can be discarded if it overflows
            this->log_WARNING_LO_LineTruncated();
            m_lineBufSize = 0;
        }
    }

    this->bufferReturn_out(0, buffer);
}

// ----------------------------------------------------------------------
// processLine — strips the "LOG: " prefix and splits glued records
// ----------------------------------------------------------------------

void SatnogsComHandler::processLine(const char* line, U32 len) {
    // Every valid SatNOGS line starts with "LOG: "
    if (len < LOG_PREFIX_LEN || strncmp(line, "LOG: ", LOG_PREFIX_LEN) != 0) {
        return;
    }

    // The radio sometimes emits records without a separating newline
    // (e.g. "...iface=2LOG: TX UHF..." or "POWERLOG: <hex>"), so split the
    // content on every embedded "LOG: " marker.
    const char* p = line + LOG_PREFIX_LEN;
    char piece[LINE_BUFFER_SIZE];

    while (p != nullptr) {
        const char* next = strstr(p, "LOG: ");
        if (next == nullptr) {
            processContent(p);
            break;
        }
        U32 pieceLen = static_cast<U32>(next - p);
        if (pieceLen >= sizeof(piece)) {
            pieceLen = sizeof(piece) - 1;
        }
        memcpy(piece, p, pieceLen);
        piece[pieceLen] = '\0';
        if (pieceLen > 0) {
            processContent(piece);
        }
        p = next + LOG_PREFIX_LEN;
    }
}

// ----------------------------------------------------------------------
// processContent — handles one record's content
// ----------------------------------------------------------------------

void SatnogsComHandler::processContent(const char* content) {
    m_linesReceived++;
    this->tlmWrite_LinesReceived(m_linesReceived);

    // TX UHF notification: "TX UHF: len=N iface=N" (ends any open section)
    if (strncmp(content, "TX UHF:", 7) == 0) {
        flushSection();
        U32 txLen = 0, iface = 0;
        if (parseTxUhf(content, txLen, iface)) {
            m_txUhfCount++;
            this->tlmWrite_TxUhfCount(m_txUhfCount);
            this->log_ACTIVITY_LO_TxDetected(txLen, iface);
        }
        return;
    }

    // Section headers start hex-payload accumulation
    struct { const char* name; Section section; U8 id; } sections[] = {
        {"POWER",           SECTION_POWER,      1},
        {"RADIO",           SECTION_RADIO,      2},
        {"HEALTH",          SECTION_HEALTH,     3},
        {"CONFIG",          SECTION_CONFIG,     4},
        {"FPGA",            SECTION_FPGA,       5},
        {"TIME",            SECTION_TIME,       6},
        {"BOOTLOADER INFO", SECTION_BOOTLOADER, 7},
    };
    for (auto& s : sections) {
        if (strcmp(content, s.name) == 0) {
            flushSection();
            m_section = s.section;
            m_sectionIdleTicks = 0;
            this->tlmWrite_LastSectionId(s.id);
            return;
        }
    }

    // Boot message: fires on every boot, not just the first, so a radio that
    // keeps power-cycling (e.g. brownout when the payload battery turns on) is
    // correctly counted/reported instead of falling into the SatnogsData catch-all.
    if (strncmp(content, "SatNOGS-COMMS FW:", 17) == 0) {
        m_rebootCount++;
        this->tlmWrite_RebootCount(m_rebootCount);
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

    // UHF configuration echoes → typed telemetry channels
    if (parseConfigEcho(content)) {
        return;
    }

    // Hex data line inside an open section → accumulate for the section channel
    if (m_section != SECTION_NONE && appendSectionHex(content)) {
        m_sectionIdleTicks = 0;
        return;
    }

    // Anything unrecognized goes out as an event so no data is silently lost
    Fw::LogStringArg dataArg(content);
    this->log_ACTIVITY_LO_SatnogsData(dataArg);
}

// ----------------------------------------------------------------------
// parseConfigEcho — parses UHF boot configuration lines into channels
// ----------------------------------------------------------------------

bool SatnogsComHandler::parseConfigEcho(const char* content) {
    unsigned long ul = 0;
    unsigned int u = 0;
    float f = 0.0f;
    float f2 = 0.0f;

    if (sscanf(content, "UHF RX_FREQ = %lu", &ul) == 1) {
        this->tlmWrite_UhfRxFreq(static_cast<U32>(ul));
        return true;
    }
    if (sscanf(content, "UHF TX_FREQ = %lu", &ul) == 1) {
        this->tlmWrite_UhfTxFreq(static_cast<U32>(ul));
        return true;
    }
    if (sscanf(content, "UHF TX_GAIN = %f", &f) == 1) {
        this->tlmWrite_UhfTxGain(f);
        return true;
    }
    if (sscanf(content, "UHF_GAIN0_MODE = %u", &u) == 1) {
        this->tlmWrite_UhfGain0Mode(static_cast<U8>(u));
        return true;
    }
    if (sscanf(content, "UHF_GAIN1_MODE = %u", &u) == 1) {
        this->tlmWrite_UhfGain1Mode(static_cast<U8>(u));
        return true;
    }
    if (sscanf(content, "UHF RX_MODULATION = %u", &u) == 1) {
        this->tlmWrite_UhfRxModulation(static_cast<U8>(u));
        return true;
    }
    if (sscanf(content, "UHF TX_MODULATION = %u", &u) == 1) {
        this->tlmWrite_UhfTxModulation(static_cast<U8>(u));
        return true;
    }
    if (sscanf(content, "UHF FSK_TX: baudrate=%lu mod_idx=%f shaping(BT)=%f", &ul, &f, &f2) == 3) {
        this->tlmWrite_FskTxBaudrate(static_cast<U32>(ul));
        this->tlmWrite_FskTxModIdx(f);
        this->tlmWrite_FskTxShapingBT(f2);
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------
// appendSectionHex — decodes an ASCII-hex line into the section buffer
// ----------------------------------------------------------------------

static bool hexNibble(char c, U8& out) {
    if (c >= '0' && c <= '9') {
        out = static_cast<U8>(c - '0');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = static_cast<U8>(c - 'A' + 10);
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = static_cast<U8>(c - 'a' + 10);
        return true;
    }
    return false;
}

bool SatnogsComHandler::appendSectionHex(const char* content) {
    const U32 len = static_cast<U32>(strlen(content));
    if (len == 0 || (len % 2) != 0) {
        return false;
    }

    // Validate before mutating state: the whole line must be hex
    for (U32 i = 0; i < len; i++) {
        U8 nibble = 0;
        if (!hexNibble(content[i], nibble)) {
            return false;
        }
    }

    for (U32 i = 0; i < len; i += 2) {
        if (m_sectionLen >= SECTION_BUFFER_SIZE) {
            break;  // section larger than buffer: keep first SECTION_BUFFER_SIZE bytes
        }
        U8 hi = 0, lo = 0;
        (void)hexNibble(content[i], hi);
        (void)hexNibble(content[i + 1], lo);
        m_sectionBuf[m_sectionLen++] = static_cast<U8>((hi << 4) | lo);
    }
    return true;
}

// ----------------------------------------------------------------------
// flushSection — writes the accumulated section bytes to its channel
// ----------------------------------------------------------------------

void SatnogsComHandler::flushSection() {
    if (m_section == SECTION_NONE) {
        return;
    }

    if (m_sectionLen > 0) {
        logSectionData(m_sectionBuf, m_sectionLen);

        if (m_section == SECTION_FPGA || m_section == SECTION_BOOTLOADER) {
            SatnogsSectionDataSmall small;
            for (U32 i = 0; i < SatnogsSectionDataSmall::SIZE; i++) {
                small[i] = (i < m_sectionLen) ? m_sectionBuf[i] : 0;
            }
            if (m_section == SECTION_FPGA) {
                this->tlmWrite_FpgaData(small);
            } else {
                this->tlmWrite_BootloaderData(small);
            }
        } else {
            SatnogsSectionData full;
            for (U32 i = 0; i < SatnogsSectionData::SIZE; i++) {
                full[i] = (i < m_sectionLen) ? m_sectionBuf[i] : 0;
            }
            switch (m_section) {
                case SECTION_POWER:
                    this->tlmWrite_PowerData(full);
                    break;
                case SECTION_RADIO:
                    this->tlmWrite_RadioData(full);
                    break;
                case SECTION_HEALTH:
                    this->tlmWrite_HealthData(full);
                    decodeHealth(m_sectionBuf, m_sectionLen);
                    break;
                case SECTION_CONFIG:
                    this->tlmWrite_ConfigData(full);
                    break;
                case SECTION_TIME:
                    this->tlmWrite_TimeData(full);
                    break;
                default:
                    break;
            }
        }
    }

    m_section = SECTION_NONE;
    m_sectionLen = 0;
    m_sectionIdleTicks = 0;
}

// ----------------------------------------------------------------------
// sectionName / logSectionData — echo a completed section's bytes as an
// event (in addition to the telemetry channel) so values are visible live
// during a pass, not just on packet request.
// ----------------------------------------------------------------------

const char* SatnogsComHandler::sectionName(Section s) const {
    switch (s) {
        case SECTION_POWER:      return "POWER";
        case SECTION_RADIO:      return "RADIO";
        case SECTION_HEALTH:     return "HEALTH";
        case SECTION_CONFIG:     return "CONFIG";
        case SECTION_FPGA:       return "FPGA";
        case SECTION_TIME:       return "TIME";
        case SECTION_BOOTLOADER: return "BOOTLOADER INFO";
        default:                 return "";
    }
}

void SatnogsComHandler::logSectionData(const U8* data, U32 len) {
    static const char HEX[] = "0123456789ABCDEF";
    char hex[(SECTION_HEX_EVENT_MAX_BYTES * 2) + 1];
    const U32 n = (len < SECTION_HEX_EVENT_MAX_BYTES) ? len : SECTION_HEX_EVENT_MAX_BYTES;
    for (U32 i = 0; i < n; i++) {
        hex[i * 2] = HEX[(data[i] >> 4) & 0xF];
        hex[i * 2 + 1] = HEX[data[i] & 0xF];
    }
    hex[n * 2] = '\0';

    Fw::LogStringArg sectionArg(sectionName(m_section));
    Fw::LogStringArg hexArg(hex);
    this->log_ACTIVITY_LO_SectionData(sectionArg, hexArg);
}

// ----------------------------------------------------------------------
// decodeHealth — decodes the HEALTH section into typed telemetry.
// Layout from SatNOGS-COMMS FW 1.14.102 (src/telemetry.hpp, HEALTH
// serializer): CCSDS TM header (6 bytes, APID 104), then MSB-first
// bit-packed: uptimeMs(64) bootCount(32) pcbTemp(f32) uhfPaTemp(f32)
// sbandPaTemp(f32) alerts(2) rstCause(15) powerEnables(8x1) 8x f32
// [vinV vinA fpgaA dig3v3A rf5vA emcW efusesW vbatV] pgood(4x1)
// thermal(5x1). Validated against a captured boot dump.
// ----------------------------------------------------------------------

namespace {
class BitReader {
  public:
    BitReader(const U8* data, U32 numBytes) : m_data(data), m_bits(numBytes * 8), m_pos(0) {}

    U32 read(U32 n) {  // n <= 32
        U32 v = 0;
        for (U32 i = 0; i < n; i++) {
            v <<= 1;
            if (m_pos < m_bits) {
                v |= static_cast<U32>((m_data[m_pos / 8] >> (7 - (m_pos % 8))) & 1);
            }
            m_pos++;
        }
        return v;
    }

    F32 readFloat() {
        U32 raw = read(32);
        F32 f = 0.0f;
        memcpy(&f, &raw, sizeof(f));
        return f;
    }

    void skip(U32 n) { m_pos += n; }
    bool ok() const { return m_pos <= m_bits; }

  private:
    const U8* m_data;
    U32 m_bits;
    U32 m_pos;
};
}  // namespace

void SatnogsComHandler::decodeHealth(const U8* msg, U32 len) {
    // Minimum: 6-byte header + fixed bit fields (uptime through thermal flags)
    static constexpr U32 CCSDS_HEADER_SIZE = 6;
    static constexpr U32 HEALTH_APID = 104;
    static constexpr U32 FIXED_BITS = 64 + 32 + (3 * 32) + 2 + 15 + 8 + (8 * 32) + 4 + 5;
    if (len < CCSDS_HEADER_SIZE + ((FIXED_BITS + 7) / 8)) {
        return;
    }

    // CCSDS header: version(3) type(1) secHdr(1) apid(11) ...
    const U32 apid = (static_cast<U32>(msg[0] & 0x07) << 8) | msg[1];
    if (apid != HEALTH_APID) {
        return;
    }

    BitReader r(msg + CCSDS_HEADER_SIZE, len - CCSDS_HEADER_SIZE);

    // Uptime is 64-bit milliseconds; report as seconds in a U32
    const U32 uptimeMsHi = r.read(32);
    const U32 uptimeMsLo = r.read(32);
    const U64 uptimeMs = (static_cast<U64>(uptimeMsHi) << 32) | uptimeMsLo;
    this->tlmWrite_HealthUptime(static_cast<U32>(uptimeMs / 1000U));

    this->tlmWrite_HealthBootCount(r.read(32));
    this->tlmWrite_HealthPcbTemp(r.readFloat());
    this->tlmWrite_HealthUhfPaTemp(r.readFloat());
    this->tlmWrite_HealthSbandPaTemp(r.readFloat());
    r.skip(2);  // UHF_PA / SBAND_PA alert bits
    this->tlmWrite_HealthResetCause(static_cast<U16>(r.read(15)));

    this->tlmWrite_HealthPowerEnables(static_cast<U8>(r.read(8)));
    const F32 vinVoltage = r.readFloat();
    const F32 vinCurrent = r.readFloat();
    r.readFloat();  // FPGA current (FPGA removed from this board per ICD)
    const F32 dig3v3Current = r.readFloat();
    const F32 rf5vCurrent = r.readFloat();
    const F32 emcPower = r.readFloat();
    r.readFloat();  // EFUSES power (unpopulated sensor, transmits junk)
    r.readFloat();  // VBAT voltage (unpopulated sensor, transmits junk)
    this->tlmWrite_HealthVinVoltage(vinVoltage);
    this->tlmWrite_HealthVinCurrent(vinCurrent);
    this->tlmWrite_HealthDig3v3Current(dig3v3Current);
    this->tlmWrite_HealthRf5vCurrent(rf5vCurrent);
    this->tlmWrite_HealthEmcPower(emcPower);
    this->tlmWrite_HealthPgood(static_cast<U8>(r.read(4)));
    this->tlmWrite_HealthThermalFlags(static_cast<U8>(r.read(5)));
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
// schedIn — 1Hz tick; flushes a section left open by a quiet UART
// ----------------------------------------------------------------------

void SatnogsComHandler::schedIn_handler(FwIndexType portNum, U32 context) {
    (void)portNum;
    (void)context;
    if (m_section != SECTION_NONE) {
        m_sectionIdleTicks++;
        if (m_sectionIdleTicks >= SECTION_IDLE_FLUSH_TICKS) {
            flushSection();
        }
    }
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

void SatnogsComHandler::sendToUart(const char* str) {
    Fw::Buffer buf(reinterpret_cast<U8*>(const_cast<char*>(str)),
                   static_cast<U32>(strlen(str)));
    Drv::ByteStreamStatus status = this->commandOut_out(0, buf);
    if (status != Drv::ByteStreamStatus::OP_OK) {
        this->log_WARNING_HI_SatnogsError(2);
    }
}

}  // namespace Components
