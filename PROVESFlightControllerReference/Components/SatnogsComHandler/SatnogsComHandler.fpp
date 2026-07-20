module Components {
    @ Raw decoded bytes of one SatNOGS telemetry section (zero-padded).
    @ Byte layout is the radio's native message: msgId(2) + hdr(3) + len(1) + payload.
    array SatnogsSectionData = [128] U8

    @ Raw decoded bytes of a short SatNOGS section (FPGA / BOOTLOADER INFO)
    array SatnogsSectionDataSmall = [16] U8

    passive component SatnogsComHandler {

        #----------#
        # Commands #
        #----------#

        @ Send an arbitrary command string to the SatNOGS module over UART
        sync command SEND_COMMAND(cmd: string)

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

        @ Raw telemetry/config data line from the SatNOGS module (downlinked live during a pass)
        event SatnogsData(msg: string) \
            severity activity low format "SatNOGS data: {}"

        #-------------#
        #  Telemetry  #
        #-------------#

        @ Total LOG lines received from SatNOGS
        telemetry LinesReceived: U32

        @ Number of TX UHF transmissions detected
        telemetry TxUhfCount: U32

        @ ID of last telemetry section received (1=POWER 2=RADIO 3=HEALTH 4=CONFIG 5=FPGA 6=TIME 7=BOOTLOADER)
        telemetry LastSectionId: U8

        ## UHF configuration echoed by the radio at boot (ASCII lines)
        @ UHF receive frequency in Hz
        telemetry UhfRxFreq: U32

        @ UHF transmit frequency in Hz
        telemetry UhfTxFreq: U32

        @ UHF transmit gain in dB
        telemetry UhfTxGain: F32

        @ Gain stage 0 mode (0=AUTO, 1=MANUAL)
        telemetry UhfGain0Mode: U8

        @ Gain stage 1 mode (0=AUTO, 1=MANUAL)
        telemetry UhfGain1Mode: U8

        @ UHF receive modulation index
        telemetry UhfRxModulation: U8

        @ UHF transmit modulation index
        telemetry UhfTxModulation: U8

        @ FSK TX baudrate
        telemetry FskTxBaudrate: U32

        @ FSK TX modulation index
        telemetry FskTxModIdx: F32

        @ FSK TX gaussian shaping (BT)
        telemetry FskTxShapingBT: F32

        ## Raw section payloads (hex lines decoded to bytes; decode fields on the ground per ICD)
        @ POWER section raw bytes
        telemetry PowerData: SatnogsSectionData

        @ RADIO section raw bytes
        telemetry RadioData: SatnogsSectionData

        @ HEALTH section raw bytes
        telemetry HealthData: SatnogsSectionData

        @ CONFIG section raw bytes
        telemetry ConfigData: SatnogsSectionData

        @ TIME section raw bytes
        telemetry TimeData: SatnogsSectionData

        @ FPGA section raw bytes
        telemetry FpgaData: SatnogsSectionDataSmall

        @ BOOTLOADER INFO section raw bytes
        telemetry BootloaderData: SatnogsSectionDataSmall

        ## Decoded HEALTH section fields (layout validated against
        ## SatNOGS-COMMS FW 1.14.102, hsfl/SatNOGS-COMMS-Software-MCU
        ## broncospace-delivery branch, src/telemetry.hpp)
        @ Radio MCU uptime in seconds
        telemetry HealthUptime: U32

        @ Radio MCU boot count
        telemetry HealthBootCount: U32

        @ Radio MCU hardware reset cause bits
        telemetry HealthResetCause: U16

        @ Radio PCB temperature in degrees C
        telemetry HealthPcbTemp: F32

        @ UHF power amplifier temperature in degrees C
        telemetry HealthUhfPaTemp: F32

        @ S-band power amplifier temperature in degrees C
        telemetry HealthSbandPaTemp: F32

        @ Radio input bus voltage in V
        telemetry HealthVinVoltage: F32

        @ Radio input bus current in A
        telemetry HealthVinCurrent: F32

        @ Radio 3.3V digital rail current in A
        telemetry HealthDig3v3Current: F32

        @ Radio 5V RF rail current in A
        telemetry HealthRf5vCurrent: F32

        @ Radio total power (EMC1702 sensor) in W
        telemetry HealthEmcPower: F32

        @ Power-enable flags: bit7=RF_5V bit6=FPGA_5V bit5=CAN1 bit4=CAN1_LPWR bit3=CAN2 bit2=CAN2_LPWR bit1=UHF bit0=SBAND
        telemetry HealthPowerEnables: U8

        @ Power-good flags: bit3=5V bit2=FPGA bit1=UHF bit0=SBAND
        telemetry HealthPgood: U8

        @ Thermal state flags: bit4=uhfTriggered bit3=sbandTriggered bit2=uhfSensorValid bit1=sbandSensorValid bit0=pcbSensorValid
        telemetry HealthThermalFlags: U8

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
