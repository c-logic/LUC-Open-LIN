/*
 * luc_proto.h - ASCII-Host-Protokoll (siehe docs/PROTOKOLL.md)
 */
#ifndef LUC_PROTO_H_
#define LUC_PROTO_H_

void luc_proto_init(void);

/* Hauptschleife: Befehle lesen/ausfuehren, Ereignisse ausgeben */
void luc_proto_poll(void);

#endif /* LUC_PROTO_H_ */
