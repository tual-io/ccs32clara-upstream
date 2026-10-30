/* V2G Transfer Protocol (ISO 15118-2 chapter 7.8): collecting one V2GTP message from TCP segments, and reading the
   SDP response. Plain functions without hardware or global state, so that they can be tested on a PC
   (test/host). */

#ifndef V2GTP_H
#define V2GTP_H

#include <stdint.h>

#define V2GTP_HEADER_LEN 8

/* SDP response (V2GTP payload type 0x9001), ISO 15118-2 table 15 */
#define V2GTP_SDP_SECURITY_TLS 0x00   /* secured with TLS */
#define V2GTP_SDP_SECURITY_NONE 0x10  /* no transport layer security */
#define V2GTP_SDP_TRANSPORT_TCP 0x00

struct V2gtpSdpResponse
{
   uint8_t seccIp[16];
   uint16_t seccPort;
   uint8_t security;
   uint8_t transport;
};

/* Returns 1 if p (len bytes of UDP payload) is a valid SDP response, and fills r. */
uint8_t v2gtp_parseSdpResponse(const uint8_t* p, uint16_t len, struct V2gtpSdpResponse* r);

/* Collects one V2GTP message at a time from the TCP data stream into buf. The message length comes from the V2GTP
   header (8 bytes plus the payload length), so a message may come in several segments. */
struct V2gtpRx
{
   uint8_t* buf;     /* where the message is collected */
   uint16_t size;    /* its size */
   uint32_t nextSeq; /* the TCP sequence number of the next byte expected: the ACK number to send */
   uint16_t len;     /* bytes of the current message collected so far */
   uint32_t msgLen;  /* the current message's length, once its header is in; 0 before */
   uint32_t skip;    /* bytes still to be discarded of a message too large for buf */
};

/* What v2gtpRx_segment() did (bits) */
#define V2GTPRX_STARTED   0x01 /* the first bytes of a new message went into buf: a message there before is gone */
#define V2GTPRX_COMPLETE  0x02 /* a message is complete in buf, *completeLen bytes */
#define V2GTPRX_TOO_LARGE 0x04 /* a message was larger than buf: it is discarded (and acknowledged) */
#define V2GTPRX_BAD_HEADER 0x08 /* the data didn't start with a V2GTP header: the segment is passed on as it is */
#define V2GTPRX_DUPLICATE 0x10 /* the segment was received before (retransmission): nothing new */
#define V2GTPRX_GAP       0x20 /* the segment starts beyond the next byte expected: not taken */
#define V2GTPRX_EXTRA     0x40 /* bytes after the end of a complete message in the same segment: discarded */

/* Starts collecting; firstSeq is the sequence number of the first data byte (the SYN's sequence number plus 1). */
void v2gtpRx_start(struct V2gtpRx* rx, uint8_t* buf, uint16_t size, uint32_t firstSeq);

/* Takes the n data bytes of a TCP segment with sequence number seq. Afterwards, rx->nextSeq is the ACK number to
   send. Returns V2GTPRX_... bits. */
uint8_t v2gtpRx_segment(struct V2gtpRx* rx, uint32_t seq, const uint8_t* data, uint16_t n, uint16_t* completeLen);

#endif
