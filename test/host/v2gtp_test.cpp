/* Host test of ccs/v2gtp.cpp: V2GTP message assembly from TCP segments, and the SDP response.
   Build and run on a PC: make -C test/host */

#include "v2gtp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int failures;

#define CHECK(cond)                                                                         \
   do                                                                                       \
   {                                                                                        \
      if (!(cond))                                                                          \
      {                                                                                     \
         printf("FAIL line %d: %s\n", __LINE__, #cond);                                     \
         failures++;                                                                        \
      }                                                                                     \
   } while (0)

/* A V2GTP message: header (version 1, payload type 0x8001, length) and payload bytes 0, 1, 2, ... */
static std::vector<uint8_t> message(uint32_t payloadLen, uint8_t first = 0)
{
   std::vector<uint8_t> m = { 0x01, 0xFE, 0x80, 0x01, (uint8_t)(payloadLen >> 24), (uint8_t)(payloadLen >> 16),
                              (uint8_t)(payloadLen >> 8), (uint8_t)payloadLen };
   for (uint32_t i = 0; i < payloadLen; i++)
      m.push_back((uint8_t)(first + i));
   return m;
}

static void testSingleSegment(void)
{
   uint8_t buf[1032];
   struct V2gtpRx rx;
   uint16_t len = 0;
   std::vector<uint8_t> m = message(100);

   v2gtpRx_start(&rx, buf, sizeof(buf), 1000);
   uint8_t r = v2gtpRx_segment(&rx, 1000, m.data(), (uint16_t)m.size(), &len);
   CHECK(r == (V2GTPRX_STARTED | V2GTPRX_COMPLETE));
   CHECK((size_t)len == m.size());
   CHECK(memcmp(buf, m.data(), m.size()) == 0);
   CHECK(rx.nextSeq == 1000 + m.size());
}

/* A 1 kB message in segments of every size from 1 to 600 bytes, the header split too. */
static void testSplit(void)
{
   std::vector<uint8_t> m = message(1024, 7);
   for (uint16_t seg = 1; seg <= 600; seg++)
   {
      uint8_t buf[1032];
      struct V2gtpRx rx;
      uint16_t len = 0;
      uint8_t all = 0;
      uint32_t seq = 0xFFFFFF00u; /* also across the sequence number wrap */
      v2gtpRx_start(&rx, buf, sizeof(buf), seq);
      for (size_t pos = 0; pos < m.size(); pos += seg)
      {
         uint16_t n = (uint16_t)(m.size() - pos < seg ? m.size() - pos : seg);
         uint8_t r = v2gtpRx_segment(&rx, seq + (uint32_t)pos, &m[pos], n, &len);
         if (pos + n < m.size())
            CHECK(!(r & V2GTPRX_COMPLETE));
         all |= r;
      }
      CHECK(all == (V2GTPRX_STARTED | V2GTPRX_COMPLETE));
      CHECK((size_t)len == m.size());
      CHECK(memcmp(buf, m.data(), m.size()) == 0);
      CHECK(rx.nextSeq == (uint32_t)(seq + m.size()));
   }
}

/* Retransmissions, overlaps and gaps */
static void testOrder(void)
{
   uint8_t buf[1032];
   struct V2gtpRx rx;
   uint16_t len = 0;
   std::vector<uint8_t> m = message(300);

   v2gtpRx_start(&rx, buf, sizeof(buf), 5000);
   CHECK(v2gtpRx_segment(&rx, 5000, &m[0], 100, &len) == V2GTPRX_STARTED);
   CHECK(v2gtpRx_segment(&rx, 5000, &m[0], 100, &len) == V2GTPRX_DUPLICATE); /* the same again */
   CHECK(v2gtpRx_segment(&rx, 5200, &m[200], 108, &len) == V2GTPRX_GAP);     /* 100..199 missing */
   CHECK(rx.nextSeq == 5100);
   CHECK(v2gtpRx_segment(&rx, 5050, &m[50], 150, &len) == 0); /* overlaps: only 100..199 are new */
   CHECK(rx.nextSeq == 5200);
   CHECK(v2gtpRx_segment(&rx, 5200, &m[200], 108, &len) == V2GTPRX_COMPLETE);
   CHECK((size_t)len == m.size());
   CHECK(memcmp(buf, m.data(), m.size()) == 0);
}

/* A message larger than the buffer is dropped to its end; the next one is fine. */
static void testTooLarge(void)
{
   uint8_t buf[200];
   struct V2gtpRx rx;
   uint16_t len = 0;
   std::vector<uint8_t> big = message(500);
   std::vector<uint8_t> next = message(50, 9);
   uint8_t all = 0;

   v2gtpRx_start(&rx, buf, sizeof(buf), 1);
   for (size_t pos = 0; pos < big.size(); pos += 128)
   {
      uint16_t n = (uint16_t)(big.size() - pos < 128 ? big.size() - pos : 128);
      all |= v2gtpRx_segment(&rx, 1 + (uint32_t)pos, &big[pos], n, &len);
   }
   CHECK(all == (V2GTPRX_STARTED | V2GTPRX_TOO_LARGE));
   CHECK(rx.nextSeq == 1 + big.size()); /* all acknowledged */
   CHECK(v2gtpRx_segment(&rx, rx.nextSeq, next.data(), (uint16_t)next.size(), &len) ==
         (V2GTPRX_STARTED | V2GTPRX_COMPLETE));
   CHECK((size_t)len == next.size());
   CHECK(memcmp(buf, next.data(), next.size()) == 0);
}

/* Data after the end of a message in the same segment is dropped, the message kept. */
static void testExtra(void)
{
   uint8_t buf[1032];
   struct V2gtpRx rx;
   uint16_t len = 0;
   std::vector<uint8_t> m = message(20);
   std::vector<uint8_t> two = m;
   two.insert(two.end(), m.begin(), m.end());

   v2gtpRx_start(&rx, buf, sizeof(buf), 0);
   CHECK(v2gtpRx_segment(&rx, 0, two.data(), (uint16_t)two.size(), &len) ==
         (V2GTPRX_STARTED | V2GTPRX_COMPLETE | V2GTPRX_EXTRA));
   CHECK((size_t)len == m.size());
   CHECK(rx.nextSeq == two.size());
}

/* Not a V2GTP header: passed on as it is (bounded by the buffer). */
static void testBadHeader(void)
{
   uint8_t buf[64];
   struct V2gtpRx rx;
   uint16_t len = 0;
   std::vector<uint8_t> junk(100, 0x55);

   v2gtpRx_start(&rx, buf, sizeof(buf), 0);
   CHECK(v2gtpRx_segment(&rx, 0, junk.data(), (uint16_t)junk.size(), &len) ==
         (V2GTPRX_STARTED | V2GTPRX_COMPLETE | V2GTPRX_BAD_HEADER));
   CHECK((size_t)len == sizeof(buf));
   CHECK(rx.nextSeq == junk.size());
}

static void testSdp(void)
{
   struct V2gtpSdpResponse r;
   uint8_t p[28] = { 0x01, 0xFE, 0x90, 0x01, 0, 0, 0, 20 };
   for (int i = 0; i < 16; i++)
      p[8 + i] = (uint8_t)(0xA0 + i);
   p[24] = 0xD1; /* port 0xD1C2 */
   p[25] = 0xC2;
   p[26] = 0x10; /* no TLS */
   p[27] = 0x00; /* TCP */
   CHECK(v2gtp_parseSdpResponse(p, sizeof(p), &r) == 1);
   CHECK(r.seccPort == 0xD1C2);
   CHECK(r.seccIp[0] == 0xA0 && r.seccIp[15] == 0xAF);
   CHECK(r.security == V2GTP_SDP_SECURITY_NONE);
   CHECK(r.transport == V2GTP_SDP_TRANSPORT_TCP);

   p[26] = 0x00; /* TLS only */
   CHECK(v2gtp_parseSdpResponse(p, sizeof(p), &r) == 1);
   CHECK(r.security == V2GTP_SDP_SECURITY_TLS);

   CHECK(v2gtp_parseSdpResponse(p, 27, &r) == 0); /* too short */
   p[7] = 19;
   CHECK(v2gtp_parseSdpResponse(p, sizeof(p), &r) == 0); /* wrong payload length */
   p[7] = 20;
   p[3] = 0x00;
   CHECK(v2gtp_parseSdpResponse(p, sizeof(p), &r) == 0); /* an SDP request, not a response */
   p[3] = 0x01;
   p[1] = 0xFF;
   CHECK(v2gtp_parseSdpResponse(p, sizeof(p), &r) == 0); /* not V2GTP */
}

int main(void)
{
   testSingleSegment();
   testSplit();
   testOrder();
   testTooLarge();
   testExtra();
   testBadHeader();
   testSdp();
   if (failures)
      printf("FAILED (%d)\n", failures);
   else
      printf("OK\n");
   return failures ? 1 : 0;
}
