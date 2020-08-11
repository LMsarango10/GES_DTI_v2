#ifndef _SENDDATA_H
#define _SENDDATA_H

#include "spislave.h"
#include "cyclic.h"

#if(HAS_LORA)
#include "lorawan.h"
#endif

#if(HAS_NBIOT)
#include "nbiot.h"
#endif

#ifdef HAS_DISPLAY
#include "display.h"
#endif

#ifdef HAS_SDCARD
#include "sdcard.h"
#endif

extern Ticker sendcycler;

void SendPayload(uint8_t port, sendprio_t prio);
void sendData(void);
void checkQueue();
void checkSendQueues(void);
void flushQueues();
void sendcycle(void);

#endif // _SENDDATA_H_
