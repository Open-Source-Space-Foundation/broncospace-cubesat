// ======================================================================
// \title  radfetComHandler.hpp
// \author kai
// \brief  hpp file for radfetComHandler component implementation class
// ======================================================================

#ifndef Components_radfetComHandler_HPP
#define Components_radfetComHandler_HPP

#include "PROVESFlightControllerReference/Components/radfetComHandler/radfetComHandlerComponentAc.hpp"
#include <Os/File.hpp>
#include <string>

namespace Components {

class radfetComHandler final : public radfetComHandlerComponentBase {
  public:
    // ----------------------------------------------------------------------
    // Component construction and destruction
    // ----------------------------------------------------------------------

    //! Construct radfetComHandler object
    radfetComHandler(const char* const compName  //!< The component name
    );

    //! Destroy radfetComHandler object
    ~radfetComHandler();

  private:
    // ----------------------------------------------------------------------
    // Handler implementations for typed input ports
    // ----------------------------------------------------------------------

    //! Handler implementation for dataIn
    //!
    //! Receives data from PayloadCom
    void dataIn_handler(FwIndexType portNum,  //!< The port number
                        Fw::Buffer& buffer,
                        const Drv::ByteStreamStatus& status) override;

    //! Handler implementation for schedIn
    //!
    //! Port for scheduling readings periodically
    void schedIn_handler(FwIndexType portNum,  //!< The port number
                         U32 context           //!< The call order
                         ) override;

  private:
    // ----------------------------------------------------------------------
    // Handler implementations for commands
    // ----------------------------------------------------------------------

    //! Handler implementation for command START_READINGS
    //!
    //! Start Radiation Readings
    void START_READINGS_cmdHandler(FwOpcodeType opCode,  //!< The opcode
                                   U32 cmdSeq,           //!< The command sequence number
                                   U32 interval          //!< Reading interval in seconds
                                   ) override;

    //! Handler implementation for command STOP_READINGS
    //!
    //! Stop Radiation Readings
    void STOP_READINGS_cmdHandler(FwOpcodeType opCode,  //!< The opcode
                                  U32 cmdSeq            //!< The command sequence number
                                  ) override;

    //! Handler implementation for command TAKE_READING
    //!
    //! Take immediate radiation reading
    void TAKE_READING_cmdHandler(FwOpcodeType opCode,  //!< The opcode
                                 U32 cmdSeq            //!< The command sequence number
                                 ) override;

    //! Handler implementation for command SEND_COMMAND
    //!
    //! Send command to radFET sensor
    void SEND_COMMAND_cmdHandler(FwOpcodeType opCode,  //!< The opcode
                                 U32 cmdSeq,           //!< The command sequence number
                                 const Fw::CmdStringArg& cmd
                                ) override;

    void DOWNLINK_REQUEST_cmdHandler(FwOpcodeType opCode, 
                                     U32 cmdSeq,
                                     U32 numPackets
                                    ) override;




    void processSensorData();
    bool accumulateSensorData(const U8* data, U32 size);
    void clearDataBuffer();
    bool parseRadiationData(const U8* data, U32 size, U32& rawCounts);
    bool validateRawData(U32 rawCounts);
    void takeRadiationReading();
    void sendSensorCommand(const char* command);
    //U32 calculateChecksum(void* data, U32 size);
    void removeProcessedData(U32 size);


    bool parseDownlinkPacket(const U8* data, U32 size);


    bool m_periodicReadings;
    U32 m_readingInterval;
    U32 m_readingsCount;
    //U32 m_storedReadings;  // for CADENCE
    U32 m_lastRawCounts;
    U32  m_lastReadingTimestamp;
    U32  m_packetsDownlinked;

    static constexpr U32 DATA_BUFFER_SIZE = 512;
    U8 m_dataBuffer[DATA_BUFFER_SIZE];
    U32 m_dataBufferSize;

    static constexpr U8  RESPONSE_START_MARKER = 0xAA;
    static constexpr U32 RESPONSE_SIZE         = 6;

    static constexpr U8  DOWNLINK_START_MARKER = 0xBB;
    static constexpr U8  DOWNLINK_END_MARKER   = 0xEE;

    struct RadfetReading {
        U8  moduleNum;
        U8  radfetNum;
        U16 adcValue;
        U32 timestamp;
    };

    static constexpr U8 READINGS_PER_PACKET = 20;

    struct RadfetPacket {
        U32 packetId;
        U32 timestampStart;
        U32 timestampEnd;
        U8  sampleCount;
        RadfetReading readings[READINGS_PER_PACKET];
        U32 checksum;
    };


};

}// namespace Components

#endif
