module Components {
    passive component SatnogsComHandler {

        #----------#
        # Commands #
        #----------#

        @ Send an arbitrary command string to the SatNOGS module over UART
        sync command SEND_COMMAND(cmd: string)

        @ Close the log file (call before requesting file downlink)
        sync command CLOSE_LOG()

        #----------#
        #  Events  #
        #----------#

        event BootMessage(version: string) \
            severity activity high format "SatNOGS booted: {}"

        event TxDetected(len: U32, iface: U32) \
            severity activity low format "SatNOGS TX UHF: len={} iface={}"

        event SectionReceived(section: string) \
            severity activity low format "SatNOGS telemetry section: {}"

        event SatnogsLog(msg: string) \
            severity activity low format "SatNOGS: {}"

        event SatnogsError(error: U32) \
            severity warning high format "SatNOGS comm error code: {}"

        event LineTruncated() \
            severity warning low format "SatNOGS line exceeded buffer (truncated)"

        event LogFileOpened(path: string) \
            severity activity high format "SatNOGS log file opened: {}"

        event LogFileError(op: U32, stat: I32) \
            severity warning high format "SatNOGS log file error op={} status={}"

        event LogFileClosed() \
            severity activity high format "SatNOGS log file closed"

        #-------------#
        #  Telemetry  #
        #-------------#

        @ Total LOG lines received from SatNOGS
        telemetry LinesReceived: U32

        @ Number of TX UHF transmissions detected
        telemetry TxUhfCount: U32

        @ ID of last telemetry section received (1=POWER 2=RADIO 3=HEALTH 4=CONFIG 5=FPGA 6=TIME 7=BOOTLOADER)
        telemetry LastSectionId: U8

        #-------------#
        #    Ports    #
        #-------------#

        @ Receives data bytes from ZephyrUartDriver
        sync input port dataIn: Drv.ByteStreamData

        @ Sends command bytes to ZephyrUartDriver
        output port commandOut: Drv.ByteStreamSend

        @ 1Hz schedule tick (for health monitoring)
        sync input port schedIn: Svc.Sched

        @ Returns RX buffers back to ZephyrUartDriver
        output port bufferReturn: Fw.BufferSend

        time get port timeCaller
        command reg port cmdRegOut
        command recv port cmdIn
        command resp port cmdResponseOut
        text event port logTextOut
        event port logOut
        telemetry port tlmOut
    }
}
