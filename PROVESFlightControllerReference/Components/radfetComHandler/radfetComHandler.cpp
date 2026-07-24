// ======================================================================
// \title  radfetComHandler.cpp
// \author kai
// \brief  cpp file for radfetComHandler component implementation class
// ======================================================================

#include "PROVESFlightControllerReference/Components/radfetComHandler/radfetComHandler.hpp"

#include <Fw/Types/Assert.hpp>
#include <cstring>
#include <string>

namespace Components {

// ----------------------------------------------------------------------
// Component construction and destruction
// ----------------------------------------------------------------------

radfetComHandler::radfetComHandler(const char* const compName)
    : radfetComHandlerComponentBase(compName),
      // m_periodicReadings(false),
      // m_readingInterval(0),
      m_readingsCount(0),
      // m_storedReadings(0),
      m_lastRawCounts(0),
      m_lastReadingTimestamp(0),
      m_packetsDownlinked(0),
      // m_currentRadiation(0),
      m_downlinkExpected(0),
      m_downlinkReceived(0),
      m_downlinkTimeoutTicks(0),
      m_dataBufferSize(0),
      m_lastModuleNum(0),
      m_lastRadfetNum(0),
      m_lastMilliVolts(0),
      m_lastDoseEstimate(0),
      m_lastDoseRate(0),
      m_lastPacketId(0),
      m_lastDownlinkTimestamp(0)
// m_currentRadiation(0),
{
    memset(m_dataBuffer, 0, DATA_BUFFER_SIZE);
}

radfetComHandler::~radfetComHandler() {
    // TODO - Open file
}

static U16 readU16Be(const U8* buffer, U32 offset) {
    return static_cast<U16>((static_cast<U16>(buffer[offset]) << 8) | static_cast<U16>(buffer[offset + 1U]));
}

static U32 readU32Be(const U8* buffer, U32 offset) {
    return (static_cast<U32>(buffer[offset]) << 24) | (static_cast<U32>(buffer[offset + 1U]) << 16) |
           (static_cast<U32>(buffer[offset + 2U]) << 8) | static_cast<U32>(buffer[offset + 3U]);
}

void radfetComHandler::dataIn_handler(FwIndexType portNum, Fw::Buffer& buffer, const Drv::ByteStreamStatus& status) {
    if (status != Drv::ByteStreamStatus::OP_OK) {
        this->log_WARNING_HI_SensorError(1);
        if (buffer.isValid()) {
            this->bufferReturn_out(0, buffer);
        }
        return;
    }

    if (!buffer.isValid()) {
        return;
    }

    const U8* data = buffer.getData();
    U32 dataSize = static_cast<U32>(buffer.getSize());

    if (!accumulateSensorData(data, dataSize)) {
        processSensorData();
        clearDataBuffer();
        accumulateSensorData(data, dataSize);
    }

    processSensorData();

    this->bufferReturn_out(0, buffer);
}

void radfetComHandler::schedIn_handler(FwIndexType portNum, U32 context) {
    if (m_downlinkExpected > 0 && m_downlinkTimeoutTicks > 0) {
        m_downlinkTimeoutTicks--;
        if (m_downlinkTimeoutTicks == 0) {
            this->log_WARNING_HI_DownlinkTimeout(m_downlinkReceived, m_downlinkExpected);
            m_downlinkExpected = 0;
            m_downlinkReceived = 0;
        }
    }
}

void radfetComHandler::START_READINGS_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    // m_periodicReadings = true;
    // m_readingInterval = interval;

    // TODO OPEN FILE
    sendSensorCommand("START\n");
    this->log_ACTIVITY_HI_ReadingStarted();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void radfetComHandler::STOP_READINGS_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    // m_periodicReadings = false;
    sendSensorCommand("STOP\n");
    this->log_ACTIVITY_HI_ReadingStopped();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

/*void radfetComHandler::RUN_CYCLE_cmdHandler(FwOpcodeType opCode, U32 cmdSeq){
    sendSensorCommand("RUN\n");
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}*/

void radfetComHandler::SEND_COMMAND_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& cmd) {
    sendSensorCommand(cmd.toChar());

    Fw::LogStringArg logCmd(cmd);
    this->log_ACTIVITY_HI_CommandSuccess(logCmd);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void radfetComHandler::DOWNLINK_REQUEST_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U32 numPackets) {
    this->log_ACTIVITY_HI_DownlinkRequested(numPackets);

    m_downlinkExpected = numPackets;
    m_downlinkReceived = 0;
    m_downlinkTimeoutTicks = DOWNLINK_TIMEOUT_TICKS;
    // send DOWNLINK_REQUEST command to payload over UART
    // format: "DL:<numPackets>\n"
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "DL:%lu\n", static_cast<unsigned long>(numPackets));
    sendSensorCommand(cmd);

    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void radfetComHandler::processSensorData() {
    while (m_dataBufferSize >= RESPONSE_SIZE) {
        // check for downlink packet first (larger packets, 0xBB marker)
        if (m_dataBuffer[0] == DOWNLINK_START_MARKER) {
            static constexpr U32 RECORD_WIRE_SIZE = 24U;
            static constexpr U32 MIN_DOWNLINK_PACKET_SIZE = 1U + 4U + 4U + 4U + 1U + 4U + 1U;
            // start + packetId + timestampStart + timestampEnd + recordCount + packetChecksum + end

            if (m_dataBufferSize < MIN_DOWNLINK_PACKET_SIZE) {
                break;
            }

            const U8 recordCount = m_dataBuffer[13];
            const U32 packetSize =
                1U + 4U + 4U + 4U + 1U + (static_cast<U32>(recordCount) * RECORD_WIRE_SIZE) + 4U + 1U;

            if (m_dataBufferSize < packetSize) {
                break;
            }

            if (m_dataBuffer[packetSize - 1U] == DOWNLINK_END_MARKER) {
                parseDownlinkPacket(m_dataBuffer, packetSize);
            }

            removeProcessedData(packetSize);
            continue;
        }

        // check for single-reading packet (0xAA marker)
        if (m_dataBuffer[0] == RESPONSE_START_MARKER) {
            if (m_dataBufferSize < RESPONSE_SIZE) {
                break;  // wait for more data
            }
            U8 moduleNum = 0U;
            U8 radfetNum = 0U;
            U32 rawCounts = 0U;
            U32 adcMilliVolts = 0U;
            U32 doseEstimate = 0U;
            U32 doseRate = 0U;

            if (parseRadiationData(m_dataBuffer, RESPONSE_SIZE, moduleNum, radfetNum, rawCounts, adcMilliVolts,
                                   doseEstimate, doseRate)) {
                m_lastModuleNum = moduleNum;
                m_lastRadfetNum = radfetNum;
                m_lastRawCounts = rawCounts;
                m_lastMilliVolts = adcMilliVolts;
                m_lastDoseEstimate = doseEstimate;
                m_lastDoseRate = doseRate;
                m_lastReadingTimestamp = m_readingsCount;

                this->tlmWrite_RawCounts(rawCounts);
                this->tlmWrite_LastModuleNum(moduleNum);
                this->tlmWrite_LastRadfetNum(radfetNum);
                this->tlmWrite_LastMilliVolts(adcMilliVolts);
                this->tlmWrite_LastDoseEstimate(doseEstimate);
                this->tlmWrite_LastDoseRate(doseRate);

                this->log_ACTIVITY_HI_ReadingComplete(rawCounts, m_lastReadingTimestamp);
                this->log_ACTIVITY_LO_FullReadingReceived(moduleNum, radfetNum, rawCounts, adcMilliVolts, doseEstimate,
                                                          doseRate);

                this->tlmWrite_SensorStatus(1);
                m_readingsCount++;
                this->tlmWrite_ReadingsCount(m_readingsCount);
            }
            removeProcessedData(RESPONSE_SIZE);
            continue;
        }

        // unknown byte — slide forward to find a valid marker
        removeProcessedData(1);
    }
}

bool radfetComHandler::parseDownlinkPacket(const U8* data, U32 size) {
    static constexpr U32 RECORD_WIRE_SIZE = 24U;
    static constexpr U32 MIN_DOWNLINK_PACKET_SIZE = 1U + 4U + 4U + 4U + 1U + 4U + 1U;

    if (size < MIN_DOWNLINK_PACKET_SIZE) {
        return false;
    }

    if (data[0] != DOWNLINK_START_MARKER) {
        return false;
    }

    if (data[size - 1U] != DOWNLINK_END_MARKER) {
        return false;
    }

    U32 offset = 1U;

    const U32 packetId = readU32Be(data, offset);
    offset += 4U;

    const U32 timestampStart = readU32Be(data, offset);
    offset += 4U;

    const U32 timestampEnd = readU32Be(data, offset);
    offset += 4U;

    const U8 recordCount = data[offset++];

    const U32 expectedSize = 1U + 4U + 4U + 4U + 1U + (static_cast<U32>(recordCount) * RECORD_WIRE_SIZE) + 4U + 1U;

    if (size != expectedSize) {
        this->log_WARNING_HI_DownlinkError(2);
        return false;
    }

    this->log_ACTIVITY_LO_DownlinkPacketReceived(packetId, recordCount);

    for (U8 i = 0U; i < recordCount && i < READINGS_PER_PACKET; i++) {
        const U32 timestampMs = readU32Be(data, offset);
        offset += 4U;

        const U8 moduleNum = data[offset++];
        const U8 radfetNum = data[offset++];

        const U16 adcRawValue = readU16Be(data, offset);
        offset += 2U;

        const U32 adcMilliVolts = readU32Be(data, offset);
        offset += 4U;

        const U32 doseEstimate = readU32Be(data, offset);
        offset += 4U;

        const U32 doseRate = readU32Be(data, offset);
        offset += 4U;

        const U32 recordChecksum = readU32Be(data, offset);
        offset += 4U;

        m_lastPacketId = packetId;
        m_lastDownlinkTimestamp = timestampMs;
        m_lastModuleNum = moduleNum;
        m_lastRadfetNum = radfetNum;
        m_lastRawCounts = static_cast<U32>(adcRawValue);
        m_lastMilliVolts = adcMilliVolts;
        m_lastDoseEstimate = doseEstimate;
        m_lastDoseRate = doseRate;

        this->tlmWrite_LastPacketId(packetId);
        this->tlmWrite_LastDownlinkTimestamp(timestampMs);
        this->tlmWrite_LastModuleNum(moduleNum);
        this->tlmWrite_LastRadfetNum(radfetNum);
        this->tlmWrite_RawCounts(static_cast<U32>(adcRawValue));
        this->tlmWrite_LastMilliVolts(adcMilliVolts);
        this->tlmWrite_LastDoseEstimate(doseEstimate);
        this->tlmWrite_LastDoseRate(doseRate);

        this->log_ACTIVITY_LO_RawDataReceived(static_cast<U32>(adcRawValue));
        this->log_ACTIVITY_LO_FullReadingReceived(moduleNum, radfetNum, static_cast<U32>(adcRawValue), adcMilliVolts,
                                                  doseEstimate, doseRate);

        static_cast<void>(recordChecksum);
    }

    const U32 packetChecksum = readU32Be(data, offset);
    offset += 4U;

    static_cast<void>(timestampStart);
    static_cast<void>(timestampEnd);
    static_cast<void>(packetChecksum);

    m_packetsDownlinked++;
    this->tlmWrite_PacketsDownlinked(m_packetsDownlinked);

    m_downlinkReceived++;
    if (m_downlinkExpected > 0 && m_downlinkReceived >= m_downlinkExpected) {
        this->log_ACTIVITY_HI_DownlinkComplete(m_downlinkReceived);
        m_downlinkExpected = 0;
        m_downlinkReceived = 0;
        m_downlinkTimeoutTicks = 0;
    }
    return true;
}

bool radfetComHandler::parseRadiationData(const U8* data,
                                          U32 size,
                                          U8& moduleNum,
                                          U8& radfetNum,
                                          U32& rawCounts,
                                          U32& adcMilliVolts,
                                          U32& doseEstimate,
                                          U32& doseRate) {
    if (size < RESPONSE_SIZE) {
        return false;
    }

    if (data[0] != RESPONSE_START_MARKER) {
        return false;
    }

    U8 calculatedChecksum = 0U;
    for (U32 i = 0U; i < RESPONSE_SIZE - 1U; i++) {
        calculatedChecksum ^= data[i];
    }

    if (calculatedChecksum != data[RESPONSE_SIZE - 1U]) {
        this->log_WARNING_HI_SensorError(2);
        return false;
    }

    moduleNum = data[1];
    radfetNum = data[2];

    rawCounts = readU16Be(data, 3);
    adcMilliVolts = readU32Be(data, 5);
    doseEstimate = readU32Be(data, 9);
    doseRate = readU32Be(data, 13);

    return true;
}

bool radfetComHandler::accumulateSensorData(const U8* data, U32 size) {
    if (m_dataBufferSize + size > DATA_BUFFER_SIZE) {
        return false;
    }

    memcpy(&m_dataBuffer[m_dataBufferSize], data, size);
    m_dataBufferSize += size;
    return true;
}

void radfetComHandler::clearDataBuffer() {
    m_dataBufferSize = 0;
    memset(m_dataBuffer, 0, DATA_BUFFER_SIZE);
}

void radfetComHandler::removeProcessedData(U32 size) {
    if (size >= m_dataBufferSize) {
        clearDataBuffer();
    } else {
        memmove(m_dataBuffer, &m_dataBuffer[size], m_dataBufferSize - size);
        m_dataBufferSize -= size;
    }
}

// TODO Validate raw data (e.g. check for expected ranges, consistency with previous readings, etc.)

// TODO Convert to Dose

// void radfetComHandler::takeRadiationReading() {
//    sendSensorCommand("MEASURE");
//}

void radfetComHandler::sendSensorCommand(const char* command) {
    Fw::Buffer commandBuffer(reinterpret_cast<U8*>(const_cast<char*>(command)), strlen(command));

    Drv::ByteStreamStatus status = this->commandOut_out(0, commandBuffer);

    if (status != Drv::ByteStreamStatus::OP_OK) {
        this->log_WARNING_HI_SensorError(1);
    }
}

// TODO Store to Flash

/*U32 radfetComHandler::calculateChecksum(void* data, U32 size){
    U32 checksum = 0;
    U8* byteData = static_cast<U8*>(data);

    for(U32 i=0; i< size; i++){
        checksum += byteData[i];
    }
    return checksum;
}*/

// TODO Open Data File

}  // namespace Components
