// ======================================================================
// \title  radfetComHandler.cpp
// \author kai
// \brief  cpp file for radfetComHandler component implementation class
// ======================================================================

#include "PROVESFlightControllerReference/Components/radfetComHandler/radfetComHandler.hpp"
#include <Fw/Types/Assert.hpp>
#include <Os/File.hpp>
#include <cstring>
#include <string>

namespace Components {

// ----------------------------------------------------------------------
// Component construction and destruction
// ----------------------------------------------------------------------

radfetComHandler ::radfetComHandler(const char* const compName) : radfetComHandlerComponentBase(compName),
    m_periodicReadings(false),
    m_readingInterval(0),
    m_readingsCount(0),
    //m_storedReadings(0),
    m_lastRawCounts(0),
    m_lastReadingTimestamp(0),
    m_packetsDownlinked(0),
    m_dataBufferSize(0)
    //m_currentRadiation(0),
{
    memset(m_dataBuffer, 0, DATA_BUFFER_SIZE);
}

radfetComHandler ::~radfetComHandler() {
//TODO - Open file
}

void radfetComHandler ::dataIn_handler(FwIndexType portNum, Fw::Buffer& buffer, const Drv::ByteStreamStatus& status){
    if(status != Drv::ByteStreamStatus::OP_OK){
        this->log_WARNING_HI_SensorError(1);
        if (buffer.isValid()) {
            this->bufferReturn_out(0, buffer);
        }
        return;
    }

    if (!buffer.isValid()){
        return;
    }

    const U8* data = buffer.getData();
    U32 dataSize = static_cast<U32>(buffer.getSize());

    if (!accumulateSensorData(data, dataSize)){
        processSensorData();
        clearDataBuffer();
        accumulateSensorData(data, dataSize);
    }

    processSensorData();

    this->bufferReturn_out(0, buffer);

}

void radfetComHandler::schedIn_handler(FwIndexType portNum, U32 context){
    if (m_periodicReadings){
        takeRadiationReading();
    }
}

void radfetComHandler::START_READINGS_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U32 interval){
    m_periodicReadings = true;
    m_readingInterval = interval;

    //TODO OPEN FILE
    sendSensorCommand("START");
    this->log_ACTIVITY_HI_ReadingStarted(interval);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void radfetComHandler::STOP_READINGS_cmdHandler(FwOpcodeType opCode, U32 cmdSeq){
    m_periodicReadings = false;
    sendSensorCommand("STOP");
    this->log_ACTIVITY_HI_ReadingStopped();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void radfetComHandler::TAKE_READING_cmdHandler(FwOpcodeType opCode, U32 cmdSeq){
    takeRadiationReading();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void radfetComHandler::SEND_COMMAND_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& cmd){
    sendSensorCommand(cmd.toChar());

    Fw::LogStringArg logCmd(cmd);
    this->log_ACTIVITY_HI_CommandSuccess(logCmd);
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void radfetComHandler::DOWNLINK_REQUEST_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U32 numPackets) {

    this->log_ACTIVITY_HI_DownlinkRequested(numPackets);

    // send DOWNLINK_REQUEST command to payload over UART
    // format: "DL:<numPackets>\n"
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "DL:%lu\n", static_cast<unsigned long>(numPackets));
    sendSensorCommand(cmd);

    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void radfetComHandler::processSensorData(){
    while(m_dataBufferSize >= RESPONSE_SIZE){
    // check for downlink packet first (larger packets, 0xBB marker)
        if (m_dataBuffer[0] == DOWNLINK_START_MARKER) {
            U32 packetSize = sizeof(RadfetPacket) + 2;  // +2 for start/end markers
            if (m_dataBufferSize < packetSize) {
                break;  // wait for more data
            }
            // check end marker
            if (m_dataBuffer[packetSize - 1] == DOWNLINK_END_MARKER) {
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
            U32 rawCounts = 0;
            if (parseRadiationData(m_dataBuffer, m_dataBufferSize, rawCounts)) {
                this->tlmWrite_RawCounts(rawCounts);
                U32 timestamp = m_readingsCount;
                this->log_ACTIVITY_HI_ReadingComplete(rawCounts, timestamp);
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
    if (size < sizeof(RadfetPacket) + 2) return false;
    if (data[0] != DOWNLINK_START_MARKER) return false;
    if (data[size - 1] != DOWNLINK_END_MARKER) return false;

    RadfetPacket pkt;
    memcpy(&pkt, &data[1], sizeof(pkt));

    // verify checksum
    U32 chk = 0;
    const U8* b = reinterpret_cast<const U8*>(&pkt);
    for (U32 i = 0; i < sizeof(pkt) - sizeof(pkt.checksum); i++) {
        chk = (chk * 33U) ^ b[i];
    }
    if (chk != pkt.checksum) {
        this->log_WARNING_HI_DownlinkError(1);
        return false;
    }

    this->log_ACTIVITY_LO_DownlinkPacketReceived(pkt.packetId, pkt.sampleCount);

    // log individual readings
    for (U8 i = 0; i < pkt.sampleCount && i < READINGS_PER_PACKET; i++) {
        this->log_ACTIVITY_LO_RawDataReceived(pkt.readings[i].adcValue);
    }

    m_packetsDownlinked++;
    this->tlmWrite_PacketsDownlinked(m_packetsDownlinked);

    return true;
}

bool radfetComHandler::parseRadiationData(const U8* data, U32 size, U32& rawCounts){
    if (size < RESPONSE_SIZE){
        return false;
    }

    if(data[0] != RESPONSE_START_MARKER){
        return false;
    }

    // U8 moduleNum = data[1];  // Module 1 or 2
    // U8 radfetNum = data[2];  // RADFET 1 or 2

    rawCounts = (static_cast<U32>(data[3]) << 8) | static_cast<U32>(data[4]);

    U8 calculatedChecksum = 0;
    for(U32 i = 0; i < RESPONSE_SIZE - 1; i++){
        calculatedChecksum ^= data[i];
    }

    if(calculatedChecksum != data[RESPONSE_SIZE -1]){
        this->log_WARNING_HI_SensorError(2);
        return false;
    }

    return true;
}

bool radfetComHandler::accumulateSensorData(const U8* data, U32 size){
    if(m_dataBufferSize + size > DATA_BUFFER_SIZE){
        return false;
    }

    memcpy(&m_dataBuffer[m_dataBufferSize], data, size);
    m_dataBufferSize += size;
    return true;
}

void radfetComHandler::clearDataBuffer(){
    m_dataBufferSize = 0;
    memset(m_dataBuffer, 0, DATA_BUFFER_SIZE);
}

void radfetComHandler::removeProcessedData(U32 size){
    if(size >= m_dataBufferSize){
        clearDataBuffer();
    }else{
        memmove(m_dataBuffer, &m_dataBuffer[size], m_dataBufferSize - size);
        m_dataBufferSize -= size;
    }
}

//TODO Validate raw data (e.g. check for expected ranges, consistency with previous readings, etc.)

// TODO Convert to Dose

void radfetComHandler::takeRadiationReading(){
    sendSensorCommand("MEASURE");
}

void radfetComHandler::sendSensorCommand(const char* command){
    Fw::Buffer commandBuffer(reinterpret_cast<U8*>(const_cast<char*>(command)), strlen(command));

    Drv::ByteStreamStatus status = this->commandOut_out(0, commandBuffer);

    if (status != Drv::ByteStreamStatus::OP_OK) {
        this->log_WARNING_HI_SensorError(1);
    }
}

//TODO Store to Flash

/*U32 radfetComHandler::calculateChecksum(void* data, U32 size){
    U32 checksum = 0;
    U8* byteData = static_cast<U8*>(data);

    for(U32 i=0; i< size; i++){
        checksum += byteData[i];
    }
    return checksum;
}*/

//TODO Open Data File

}
