#include "ccs32_globals.h"
#include "v2gtp.h"

/* Todo: implement a retry strategy, to cover the situation that single packets are lost on the way. */

#define NEXT_TCP 0x06 /* the next protocol is TCP */

#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_TRANSMIT_PACKET_LEN 200
/* The maximum segment size we announce: what fits into the Ethernet receive buffer after the Ethernet (14), IPv6
   (40) and TCP (20) headers, keeping 12 bytes for TCP options such as timestamps. */
#define TCP_MSS (MY_ETH_RECEIVE_BUFFER_LEN - 14 - 40 - 20 - 12)

#define TCP_ACK_TIMEOUT_MS 100 /* if for 100ms no ACK is received, we retry the transmission */
#define TCP_MAX_NUMBER_OF_RETRANSMISSIONS 40 /* allow 40 retries with 100ms cycle, to bridge 4 seconds of broken line. */
#define TCP_ACTIVITY_TIMER_START (5*33) /* 5 seconds */

#define TCP_STATE_CLOSED 0
#define TCP_STATE_SYN_SENT 1
#define TCP_STATE_ESTABLISHED 2

static uint16_t tcpActivityTimer;
static uint32_t lastUnackTransmissionTime = 0;
static uint8_t retryCounter = 0;
static uint8_t TcpIpRequestLen;
static uint8_t* TcpIpRequest = &myethtransmitbuffer[14];
static uint8_t TcpTransmitPacketLen;
static uint8_t* TcpTransmitPacket = &TcpIpRequest[40];

uint8_t tcpHeaderLen;
uint8_t tcpPayloadLen;
uint8_t* tcpPayload = &TcpTransmitPacket[20];
uint16_t tcp_rxdataLen=0;
uint8_t tcp_rxdata[TCP_RX_DATA_LEN]; /* dedicated receive buffer for TCP data with correct ports */
static struct V2gtpRx tcpRx; /* collects one V2GTP message in tcp_rxdata, possibly from several segments */

static uint8_t tcpState = TCP_STATE_CLOSED;

static uint32_t TcpSeqNr=200; /* a "random" start sequence number */
static uint32_t TcpAckNr;
static uint32_t tcp_debug_totalRetryCounter;

/*** local function prototypes ****************************************************/

static void tcp_packRequestIntoEthernet(void);
static void tcp_packRequestIntoIp(void);
static void tcp_prepareTcpHeader(uint8_t tcpFlag);
static void tcp_sendAck(void);
static void tcp_sendFirstAck(void);

/*** functions *********************************************************************/
uint32_t tcp_getTotalNumberOfRetries(void) {
  return tcp_debug_totalRetryCounter;
}

void evaluateTcpPacket(void)
{
   uint8_t flags;
   uint32_t remoteSeqNr;
   uint32_t remoteAckNr;
   uint16_t sourcePort, destinationPort, pLen, hdrLen, tmpPayloadLen, payloadOffset, available, completeLen;
   uint8_t rx;

   /* todo: check the IP addresses, checksum etc */
   nTcpPacketsReceived++;
   pLen =  (((uint16_t)myethreceivebuffer[18])<<8) +  myethreceivebuffer[19]; /* length of the IP payload */
   hdrLen=(myethreceivebuffer[66]>>4) * 4; /* header length in byte */
   //log_v("pLen=%d, hdrLen=%d", pLen, hdrLen);
   if (pLen>=hdrLen)
   {
      tmpPayloadLen = pLen - hdrLen;
   }
   else
   {
      tmpPayloadLen = 0; /* no TCP payload data */
   }
   sourcePort =      (((uint16_t)myethreceivebuffer[54])<<8) +  myethreceivebuffer[55];
   destinationPort = (((uint16_t)myethreceivebuffer[56])<<8) +  myethreceivebuffer[57];
   if ((sourcePort != seccTcpPort) || (destinationPort != evccPort))
   {
      addToTrace(MOD_TCP, "[TCP] wrong port.");
      log_v("%d %d %d %d",sourcePort,seccTcpPort,destinationPort, evccPort   );
      return; /* wrong port */
   }
   tcpActivityTimer=TCP_ACTIVITY_TIMER_START;
   remoteSeqNr =
      (((uint32_t)myethreceivebuffer[58])<<24) +
      (((uint32_t)myethreceivebuffer[59])<<16) +
      (((uint32_t)myethreceivebuffer[60])<<8) +
      (((uint32_t)myethreceivebuffer[61]));
   remoteAckNr =
      (((uint32_t)myethreceivebuffer[62])<<24) +
      (((uint32_t)myethreceivebuffer[63])<<16) +
      (((uint32_t)myethreceivebuffer[64])<<8) +
      (((uint32_t)myethreceivebuffer[65]));
   flags = myethreceivebuffer[67];
   if (flags == TCP_FLAG_SYN+TCP_FLAG_ACK)   /* This is the connection setup response from the server. */
   {
      if (tcpState == TCP_STATE_SYN_SENT)
      {
         TcpSeqNr = remoteAckNr; /* The sequence number of our next transmit packet is given by the received ACK number. */
         TcpAckNr = remoteSeqNr+1; /* The ACK number of our next transmit packet is one more than the received seq number. */
         v2gtpRx_start(&tcpRx, tcp_rxdata, TCP_RX_DATA_LEN, TcpAckNr);
         tcp_rxdataLen = 0;
         setCheckpoint(303);
         tcpState = TCP_STATE_ESTABLISHED;
         tcp_sendFirstAck();
         connMgr_TcpOk();
         addToTrace(MOD_TCP, "[TCP] connected.");
      }
      return;
   }
   /* It is no connection setup. We can have the following situations here: */
   if (tcpState != TCP_STATE_ESTABLISHED)
   {
      /* received something while the connection is closed. Just ignore it. */
      addToTrace(MOD_TCP, "[TCP] ignore, not connected.");
      return;
   }
   /* It can be an ACK, or a data package, or a combination of both. We treat the ACK and the data independent from each other,
     to treat each combination. */
   //log_v("L=%d", tmpPayloadLen);
   if (tmpPayloadLen>0)
   {
      /* This is a data transfer packet. The payload starts after the TCP header, whose length (with any options) is
         in its data offset field. Take only what is really in the receive buffer: qca7000.cpp cuts a frame longer
         than the buffer, and then only the part received is acknowledged, so the charger sends the rest again. */
      payloadOffset = 54 + hdrLen;
      available = (myethreceivebufferLen > payloadOffset) ? (myethreceivebufferLen - payloadOffset) : 0;
      if (tmpPayloadLen > available)
      {
         addToTrace(MOD_TCP, "[TCP] Segment longer than the receive buffer. Taking the part that fits.");
         tmpPayloadLen = available;
      }
      /* Fix for https://github.com/uhi22/ccs32clara/issues/15. We explicitely need to copy the data here,
      because the application will look asynchronously on the tcp_rxdata, and if in between some other received
      data would end up in the myethreceivebuffer (e.g. neighbour solicitation, or TCP traffic with other ports),
      the overwritten myethreceivebuffer would lead to not seeing the tcp_rxdata anymore. The V2GTP message may come
      in several segments; tcp_rxdataLen is set when it is complete. */
      completeLen = 0;
      rx = v2gtpRx_segment(&tcpRx, remoteSeqNr, &myethreceivebuffer[payloadOffset], tmpPayloadLen, &completeLen);
      connMgr_TcpOk();
      if (rx & V2GTPRX_STARTED)
      {
         if (tcp_rxdataLen > 0)
            addToTrace(MOD_TCP, "[TCP] The previous message was not processed yet. It is dropped.");
         tcp_rxdataLen = 0;
      }
      if (rx & V2GTPRX_TOO_LARGE)
         addToTrace(MOD_TCP, "[TCP] V2GTP message larger than the receive buffer. It is dropped.");
      if (rx & V2GTPRX_GAP)
         addToTrace(MOD_TCP, "[TCP] Segment out of order. Not taken.");
      if (rx & V2GTPRX_EXTRA)
         addToTrace(MOD_TCP, "[TCP] Data after the end of the V2GTP message. Dropped.");
      if (rx & V2GTPRX_COMPLETE)
      {
         tcp_rxdataLen = completeLen; /* provide the received message to the application */
         addToTrace(MOD_TCPTRAFFIC, "Data received: ", tcp_rxdata, tcp_rxdataLen);
      }
      TcpAckNr = tcpRx.nextSeq; /* acknowledges everything received in sequence so far */
      tcp_sendAck();
   }
   if (flags & TCP_FLAG_ACK)
   {
      nTcpPacketsReceived+=1000;
      TcpSeqNr = remoteAckNr; /* The sequence number of our next transmit packet is given by the received ACK number. */
      lastUnackTransmissionTime = 0; /* mark timer as "not used", because we received the ACK */
      retryCounter = 0;
   }
}

void tcp_connect(void)
{
   addToTrace(MOD_TCP, "[TCP] Checkpoint301: connecting");
   setCheckpoint(301);
   TcpTransmitPacket[20] = 0x02; /* options: 12 bytes, as in a Win10 notebook trace, with our own MSS and window scale */
   TcpTransmitPacket[21] = 0x04; /* maximum segment size: what our receive buffer holds */
   TcpTransmitPacket[22] = (uint8_t)(TCP_MSS >> 8);
   TcpTransmitPacket[23] = (uint8_t)(TCP_MSS);

   TcpTransmitPacket[24] = 0x01;
   TcpTransmitPacket[25] = 0x03; /* window scale: shift 0, so the window below means bytes */
   TcpTransmitPacket[26] = 0x03;
   TcpTransmitPacket[27] = 0x00;

   TcpTransmitPacket[28] = 0x01;
   TcpTransmitPacket[29] = 0x01;
   TcpTransmitPacket[30] = 0x04;
   TcpTransmitPacket[31] = 0x02;

   tcpHeaderLen = 32; /* 20 bytes normal header, plus 12 bytes options */
   tcpPayloadLen = 0;   /* only the TCP header, no data is in the connect message. */
   tcp_prepareTcpHeader(TCP_FLAG_SYN);
   tcp_packRequestIntoIp();
   tcpState = TCP_STATE_SYN_SENT;
   tcpActivityTimer=TCP_ACTIVITY_TIMER_START;
}

static void tcp_sendFirstAck(void)
{
   addToTrace(MOD_TCP, "[TCP] sending first ACK");
   tcpHeaderLen = 20; /* 20 bytes normal header, no options */
   tcpPayloadLen = 0;   /* only the TCP header, no data is in the first ACK message. */
   tcp_prepareTcpHeader(TCP_FLAG_ACK);
   tcp_packRequestIntoIp();
}

static void tcp_sendAck(void)
{
   addToTrace(MOD_TCP, "[TCP] sending ACK");
   tcpHeaderLen = 20; /* 20 bytes normal header, no options */
   tcpPayloadLen = 0;   /* only the TCP header, no data is in the first ACK message. */
   tcp_prepareTcpHeader(TCP_FLAG_ACK);
   tcp_packRequestIntoIp();
}

void tcp_transmit(void)
{
   if (tcpState == TCP_STATE_ESTABLISHED)
   {
      //addToTrace("[TCP] sending data");
      tcpHeaderLen = 20; /* 20 bytes normal header, no options */
      if (tcpPayloadLen+tcpHeaderLen<TCP_TRANSMIT_PACKET_LEN)
      {
          /* The packet fits into our transmit buffer. */
          addToTrace(MOD_TCPTRAFFIC, "TCP will transmit:", tcpPayload, tcpPayloadLen);
          tcp_prepareTcpHeader(TCP_FLAG_PSH + TCP_FLAG_ACK); /* data packets are always sent with flags PUSH and ACK. */
          tcp_packRequestIntoIp();
          lastUnackTransmissionTime = rtc_get_ms(); /* record the time of transmission, to be able to detect the timeout */
          retryCounter = TCP_MAX_NUMBER_OF_RETRANSMISSIONS; /* Allow n retries of the same packet */
      }
      else
      {
         addToTrace(MOD_TCP, "Error: tcpPayload and header do not fit into TcpTransmitPacket.");
      }
   }
}


void tcp_testSendData(void)
{
   if (tcpState == TCP_STATE_ESTABLISHED)
   {
      addToTrace(MOD_TCP, "[TCP] sending data");
      tcpHeaderLen = 20; /* 20 bytes normal header, no options */
      tcpPayloadLen = 3;   /* demo length */
      TcpTransmitPacket[tcpHeaderLen] = 0x55; /* demo data */
      TcpTransmitPacket[tcpHeaderLen+1] = 0xAA; /* demo data */
      TcpTransmitPacket[tcpHeaderLen+2] = 0xBB; /* demo data */
      tcp_prepareTcpHeader(TCP_FLAG_PSH + TCP_FLAG_ACK); /* data packets are always sent with flags PUSH and ACK. */
      tcp_packRequestIntoIp();
   }
}


static void tcp_prepareTcpHeader(uint8_t tcpFlag)
{
   uint16_t checksum;

   // # TCP header needs at least 24 bytes:
   // 2 bytes source port
   // 2 bytes destination port
   // 4 bytes sequence number
   // 4 bytes ack number
   // 4 bytes DO/RES/Flags/Windowsize
   // 2 bytes checksum
   // 2 bytes urgentPointer
   // n*4 bytes options/fill (empty for the ACK frame and payload frames)
   TcpTransmitPacket[0] = (uint8_t)(evccPort >> 8); /* source port */
   TcpTransmitPacket[1] = (uint8_t)(evccPort);
   TcpTransmitPacket[2] = (uint8_t)(seccTcpPort >> 8); /* destination port */
   TcpTransmitPacket[3] = (uint8_t)(seccTcpPort);

   TcpTransmitPacket[4] = (uint8_t)(TcpSeqNr>>24); /* sequence number */
   TcpTransmitPacket[5] = (uint8_t)(TcpSeqNr>>16);
   TcpTransmitPacket[6] = (uint8_t)(TcpSeqNr>>8);
   TcpTransmitPacket[7] = (uint8_t)(TcpSeqNr);

   TcpTransmitPacket[8] = (uint8_t)(TcpAckNr>>24); /* ack number */
   TcpTransmitPacket[9] = (uint8_t)(TcpAckNr>>16);
   TcpTransmitPacket[10] = (uint8_t)(TcpAckNr>>8);
   TcpTransmitPacket[11] = (uint8_t)(TcpAckNr);
   TcpTransmitPacketLen = tcpHeaderLen + tcpPayloadLen;
   TcpTransmitPacket[12] = (tcpHeaderLen/4) << 4; /* High-nibble: DataOffset in 4-byte-steps. Low-nibble: Reserved=0. */

   TcpTransmitPacket[13] = tcpFlag;
#define TCP_RECEIVE_WINDOW TCP_RX_DATA_LEN /* number of octets we are able to receive: one V2GTP message */
   TcpTransmitPacket[14] = (uint8_t)(TCP_RECEIVE_WINDOW>>8);
   TcpTransmitPacket[15] = (uint8_t)(TCP_RECEIVE_WINDOW);

   // checksum will be calculated afterwards
   TcpTransmitPacket[16] = 0;
   TcpTransmitPacket[17] = 0;

   TcpTransmitPacket[18] = 0; /* 16 bit urgentPointer. Always zero in our case. */
   TcpTransmitPacket[19] = 0;

   checksum = calculateUdpAndTcpChecksumForIPv6(TcpTransmitPacket, TcpTransmitPacketLen, EvccIp, SeccIp, NEXT_TCP);
   TcpTransmitPacket[16] = (uint8_t)(checksum >> 8);
   TcpTransmitPacket[17] = (uint8_t)(checksum);
}


static void tcp_packRequestIntoIp(void)
{
   // # embeds the TCP into the lower-layer-protocol: IP, Ethernet
   uint8_t i;
   uint16_t plen;
   TcpIpRequestLen = TcpTransmitPacketLen + 8 + 16 + 16; // # IP6 header needs 40 bytes:
   //  #   4 bytes traffic class, flow
   //  #   2 bytes destination port
   //  #   2 bytes length (incl checksum)
   //  #   2 bytes checksum
   TcpIpRequest[0] = 0x60; // # traffic class, flow
   TcpIpRequest[1] = 0;
   TcpIpRequest[2] = 0;
   TcpIpRequest[3] = 0;
   plen = TcpTransmitPacketLen; // length of the payload. Without headers.
   TcpIpRequest[4] = plen >> 8;
   TcpIpRequest[5] = plen & 0xFF;
   TcpIpRequest[6] = NEXT_TCP; // next level protocol, 0x06 = TCP in this case
   TcpIpRequest[7] = 0x0A; // hop limit
   // We are the PEV. So the EvccIp is our own link-local IP address.
   //EvccIp = addressManager_getLinkLocalIpv6Address("bytearray");
   for (i=0; i<16; i++)
   {
      TcpIpRequest[8+i] = EvccIp[i]; // source IP address
   }
   for (i=0; i<16; i++)
   {
      TcpIpRequest[24+i] = SeccIp[i]; // destination IP address
   }
   //showAsHex(TcpIpRequest, TcpIpRequestLen, "TcpIpRequest");
   tcp_packRequestIntoEthernet();
}

static void tcp_packRequestIntoEthernet(void)
{
   //# packs the IP packet into an ethernet packet
   myethtransmitbufferLen = TcpIpRequestLen + 6 + 6 + 2; // # Ethernet header needs 14 bytes:
   // #  6 bytes destination MAC
   // #  6 bytes source MAC
   // #  2 bytes EtherType
   //# fill the destination MAC with the MAC of the charger
   fillDestinationMac(evseMac, 0);
   fillSourceMac(getOurMac(), 6); // bytes 6 to 11 are the source MAC
   myethtransmitbuffer[12] = 0x86; // # 86dd is IPv6
   myethtransmitbuffer[13] = 0xdd;
   myEthTransmit();
}

void tcp_Disconnect(void)
{
   /* we should normally use the FIN handshake, to tell the charger that we closed the connection.
   But for the moment, just go away silently, and use an other port for the next connection. The
   server will detect our absense sooner or later by timeout, this should be good enough. */
   tcpState = TCP_STATE_CLOSED;
   /* use a new port */
   /* But: This causes multiple open connections and the Win10 messes-up them. */
   //evccPort++;
   //if (evccPort>65000) evccPort=60000;
}

void tcp_reset(void)
{
   /* Tesla v2 (at least mine) seem to run tcp over a queue that never times out:-) It deliver packets to
   previously used/wrong ports, several days since my last visit! A brutal RST seems to do the trick,
   no more wrong-port packets delivered after this.
   TODO: also call tcp_reset() before powerOff, to make sure the last connection is also freed.
   */
   
   if (tcpState != TCP_STATE_CLOSED)
   {
      tcpHeaderLen = 20;  // no options
      tcpPayloadLen = 0;  // no payload
      TcpAckNr = 0; // not acknowledging any data
      tcp_prepareTcpHeader(TCP_FLAG_RST);
      tcp_packRequestIntoIp();
      tcpState = TCP_STATE_CLOSED;  // close connection state
   }
}

uint8_t tcp_isClosed(void)
{
   return (tcpState == TCP_STATE_CLOSED);
}

uint8_t tcp_isConnected(void)
{
   return (tcpState == TCP_STATE_ESTABLISHED);
}


void tcp_Mainfunction(void)
{
   if ((lastUnackTransmissionTime > 0) && ((rtc_get_ms() - lastUnackTransmissionTime) > TCP_ACK_TIMEOUT_MS))
   {
      if (retryCounter>0) {
        /* The maximum number of retransmissions are not yet over. We still are allowed to retransmit. */
        //Retransmit
        tcp_packRequestIntoEthernet();
        tcp_debug_totalRetryCounter++;
        lastUnackTransmissionTime = rtc_get_ms(); /* record the time of transmission, to be able to detect the timeout */
        retryCounter--;
        addToTrace(MOD_TCP, "[TCP] Last packet wasn't ACKed for 100 ms, retransmitting");
      } else {
        addToTrace(MOD_TCP, "[TCP] Giving up the retry");
      }
   }
   if (connMgr_getConnectionLevel()<50)
   {
      /* No SDP done. Means: It does not make sense to start or continue TCP. */
      lastUnackTransmissionTime = 0;
      tcp_reset();
      return;
   }
   if ((connMgr_getConnectionLevel()==50) && (tcpState == TCP_STATE_CLOSED))
   {
      /* SDP is finished, but no TCP connected yet. */
      /* use a new port */
      evccPort++;
      if (evccPort>65000) evccPort=60000;
      tcp_connect();
   }
}
