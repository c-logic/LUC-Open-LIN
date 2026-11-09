# PC-Simulator der LUC-LIN-Firmware (Linux, macOS, WSL)
CFLAGS ?= -O2 -std=c99 -Wall -Wextra
luc_sim: luc_sim.c ../../Src/luc_lin.c ../../Src/luc_proto.c ../../Inc/luc_lin.h ../../Inc/luc_proto.h ../../Inc/luc_port.h
	$(CC) $(CFLAGS) -I../../Inc -o $@ luc_sim.c ../../Src/luc_lin.c ../../Src/luc_proto.c
clean:
	rm -f luc_sim
