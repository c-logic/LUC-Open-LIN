## LUC-Open-LIN

It is a more comprehensive fork of https://github.com/uCAN-LIN/LinUSBConverter

# Common Enhancements

Enhanced checksum ID = 0x00 - 0x3f 
Classic checksum ID = 0x80 - 0xBF

# open-LIN-c implementations

add a seperate STM32F0 and STM32F1 variant
STM32F1 needs to read the UART RX byte in some cases and the register naming is different

# Monitor mode

Delivers including Checksum

l #Open in Monitor-Modus

r0228 #Receive id 0x22 Length 8
z #Accepted
t022912341E0D63023200F6 #Empfang id 0x22 t0 Id:22 Len:9 Data: 12 34 1E 0D 63 02 32 00 Cs:F6

t0a05ef01000000 #Transmit 0x20 with Classic Checksum = 0xA0 Len:5 Data: EF 01 00 00 00
z #Accepted
t0200 #No Response Data

Everything else is as usual
see https://ucandevices.github.io/ulc.html

# My testing ground

A bluepill with a T151 for the STM32F1 implementation test

A LIN(Local interconnect network) Usb Converter(LUC) from Ucandevices with a STM32L042G6GQ for the STM32F0 implementation test
