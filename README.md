# LUC-Open-LIN

It is a more comprehensive fork of https://github.com/uCAN-LIN/LinUSBConverter

Enhancments:

Enhanded Checksum ID = 0x00 - 0x3f 
Classic Checksum ID = 0x80 - 0xBF

# Monitormode

Delivers inclusive Checksum

l #Open in Monitor-Modus

r0228 #Receive id 0x22 Length 8
z #Accepted
t022912341E0D63023200F6 #Empfang id 0x22 t0 Id:22 Len:9 Data: 12 34 1E 0D 63 02 32 00 Cs:F6

t0a05ef01000000 #Transmit 0x20 with Classic Checksum = 0xA0 Len:5 Data: EF 01 00 00 00
z #Accepted
t0200 #No Response Data

