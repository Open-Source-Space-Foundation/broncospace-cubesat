module Components {
    passive component radfetComHandler {

        #----------#
        # Commands #
        #----------#

        @ Start Radiation Readings
        sync command START_READINGS()

        @ Stop Radiation Readings
        sync command STOP_READINGS()

        #@ Take radiation reading
        #sync command TAKE_READING()

        @ Send command to radFET sensor
        sync command SEND_COMMAND(cmd: string)

        @ Request latest N packets from payload storage for downlink
        sync command DOWNLINK_REQUEST(
            numPackets: U32 @< Number of latest packets to downlink
        )

        #----------#
        #  events  #
        #----------#

        event ReadingStarted() \
            severity activity high format "Started periodic RADFET cycling"
        
        event ReadingStopped() \
            severity activity high format "Stopped Radiation Readings"

        event ReadingComplete(value: U32, timestamp: U32) \
            severity activity high format "Radiation reading: {} counts at timestamp {}"

        #event StorageError(error: U32) \
            #everity warning high format "Flash storage error: {}"

        event SensorError(error: U32) \
            severity warning high format "Sensor communication error: {}"

        event CommandSuccess(cmd: string) \
            severity activity high format "Command {} sent successfully"

        event RawDataReceived(counts: U32) \
            severity activity low format "Raw sensor Counts: {}"

        event ConversionError(rawCounts: U32, errorCode: U8) \
            severity warning high format "Error with Raw Counts {} (error: {})"

        event DownlinkRequested(numPackets: U32) \
            severity activity high format "Downlink requested for {} packets"

        event DownlinkPacketReceived(packetId: U32, sampleCount: U8) \
            severity activity low format "Downlink packet {} received ({} samples)"

        event DownlinkComplete(numReceived: U32) \
            severity activity high format "Downlink complete, {} packets received"

        event DownlinkError(error: U32) \
            severity warning high format "Downlink error: {}"

        event DownlinkTimeout(received: U32, expected: U32) \
            severity warning high format "Downlink timed out: received {} of {} expected packets"

        #-------------#
        #  telemetry  #
        #-------------#

        telemetry RawCounts: U32
        telemetry SensorStatus: U8
        telemetry ReadingsCount: U32
        telemetry PacketsDownlinked: U32

        #-------------#
        #    Ports    #
        #-------------#

        @ Receives data from PayloadCom
        sync input port dataIn: Drv.ByteStreamData

        @ Sends commands to RADFET via PayloadCom
        output port commandOut: Drv.ByteStreamSend

        @ Port for scheduling readings periodically
        sync input port schedIn: Svc.Sched

        @ Return RX buffers to UART driver
        output port bufferReturn: Fw.BufferSend

        time get port timeCaller
        command reg port cmdRegOut
        command recv port cmdIn
        command resp port cmdResponseOut
        text event port logTextOut
        event port logOut
        telemetry port tlmOut
        #param get port prmGetOut
        #param set port prmSetOut
    }
}
