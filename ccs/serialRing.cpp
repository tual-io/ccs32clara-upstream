/* Non-blocking serial output.

   libopeninv's Terminal sends a line by DMA and then waits, busily, until the line before it has gone. At 921600 baud
   (8N2, about 84 bytes/ms) a 30 ms cycle that prints a few kB of trace (the TCP connection, a modem search with
   several answers) is held up for tens of ms, and with libopeninv's scheduler an overrunning task then isn't run
   again for 655 ms.

   RingTerminal::PutChar collects each line (one buffer for the main loop, one for interrupts: only the scheduler's
   interrupt prints) and puts whole lines into a ring buffer. DMA2 channel 5 sends the ring to UART4, and its interrupt
   starts the next piece. A writer never waits: a line that doesn't fit is dropped whole and counted, and once a second
   a line says how many were dropped.

   Long lines. A line longer than LINE_LEN goes into the ring in pieces. For the scheduler's interrupt that is safe:
   nothing else can write between the pieces of its line. The main loop's line can be interrupted by the scheduler's
   lines, which would land inside it, so a main-loop line that long (only terminal commands print one) is wrapped
   instead: each piece is ended with "\r\n" as a line of its own.

   Masking. The ring is shared by the main loop, the scheduler's interrupt (priority 0xE0) and this DMA interrupt
   (0xD0). Writers raise BASEPRI to 0xD0 while they copy a line into the ring (a few us), which holds off those
   interrupts and the SPI DMA interrupt, but not CAN.

   Not through the ring: the terminal's input echo and its own replies to unknown commands, and binary output
   (SendBinary). They use the same DMA channel directly, so typing on the terminal can garble a line in transit.

   test/serialRing builds this file on a PC, with the DMA and the interrupts simulated (make -C test/serialRing run). */

#ifdef SERIALRING_HOST_TEST
#include "serialRing_hosttest.h" /* test/serialRing: the DMA, the masking and the clocks, simulated */
#else
#include "ccs32_globals.h"
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/dma.h>
#endif
#include "serialRing.h"

#define LOG_RING        2048   /* bytes in the ring: 24 ms of output at 921600 baud */
#define RING_LIMIT      (LOG_RING - 3) /* every write but a line end stays below this, so a line end always fits */
#define LINE_LEN        128    /* a line longer than this is sent in pieces */
#define LOG_MASK_LEVEL  0xD0   /* BASEPRI while writing: holds off priorities 0xD0 to 0xFF */

static uint8_t ring[LOG_RING];
static volatile uint16_t head;   /* next byte written */
static volatile uint16_t tail;   /* next byte sent */
static volatile uint16_t dmaLen; /* bytes the DMA is sending from tail; 0 when idle */

struct LineBuf
{
   char buf[LINE_LEN];
   uint8_t len;
   uint8_t continued; /* a piece of this line has already gone into the ring */
   uint8_t broken;    /* a piece didn't fit: the rest of the line is discarded */
   uint8_t dropLf;    /* a wrapped piece ended with the line's "\r": its "\n" is already written */
};
static LineBuf mainLine, isrLine;

static uint32_t droppedTotal;   /* lines dropped since power-on */
static uint16_t droppedPending; /* since the last report */
static uint8_t dropReportCount;

#ifndef SERIALRING_HOST_TEST /* (the host test has its own) */
static inline uint32_t maskLog(void)
{
   uint32_t old, level = LOG_MASK_LEVEL;
   __asm__ volatile ("mrs %0, basepri" : "=r"(old));
   __asm__ volatile ("msr basepri_max, %0" : : "r"(level) : "memory");
   return old;
}

static inline void unmaskLog(uint32_t old)
{
   __asm__ volatile ("msr basepri, %0" : : "r"(old) : "memory");
}

/* The active exception: 0 in the main loop. */
static inline uint32_t currentIpsr(void)
{
   uint32_t ipsr;
   __asm__ volatile ("mrs %0, ipsr" : "=r"(ipsr));
   return ipsr;
}
#endif

static inline uint16_t ringUsed(void)
{
   return (uint16_t)((head + LOG_RING - tail) % LOG_RING);
}

/* Starts the DMA on the next contiguous piece of the ring, if it is idle. Called masked, or from its interrupt. */
static void startDma(void)
{
   uint16_t h = head, t = tail, n;

   if (dmaLen != 0 || h == t)
      return;
   n = h > t ? h - t : LOG_RING - t;
   dma_disable_channel(DMA2, DMA_CHANNEL5);
   dma_set_memory_address(DMA2, DMA_CHANNEL5, (uint32_t)(uintptr_t)&ring[t]);
   dma_set_number_of_data(DMA2, DMA_CHANNEL5, n);
   dma_clear_interrupt_flags(DMA2, DMA_CHANNEL5, DMA_TCIF);
   dma_enable_transfer_complete_interrupt(DMA2, DMA_CHANNEL5);
   dmaLen = n;
   dma_enable_channel(DMA2, DMA_CHANNEL5);
}

extern "C" void dma2_channel4_5_isr(void)
{
   if (dma_get_interrupt_flag(DMA2, DMA_CHANNEL5, DMA_TCIF))
   {
      dma_clear_interrupt_flags(DMA2, DMA_CHANNEL5, DMA_TCIF);
      tail = (uint16_t)((tail + dmaLen) % LOG_RING);
      dmaLen = 0;
      startDma();
   }
}

static inline void ringPut(uint8_t c)
{
   uint16_t h = head;
#ifdef SERIALRING_HOST_TEST
   hostPreemptionPoint(); /* the host test may run an interrupt here, if the masking allows it */
   h = head;
#endif
   ring[h] = c;
   head = (uint16_t)(h + 1 == LOG_RING ? 0 : h + 1);
}

/* Copies n bytes into the ring if they fit below limit. Called masked. */
static uint8_t ringWrite(const char* data, uint16_t n, uint16_t limit)
{
   if (ringUsed() + n > limit)
      return 0;
   for (uint16_t i = 0; i < n; i++)
      ringPut((uint8_t)data[i]);
   return 1;
}

static void countDrop(void)
{
   droppedTotal++;
   if (droppedPending < 0xFFFF)
      droppedPending++;
}

/* Puts one piece of a line (all of it, unless the line is longer than LINE_LEN) into the ring. A line that doesn't
   fit is dropped whole. Only an interrupt's line longer than LINE_LEN, whose first piece has already gone, can be
   cut: a further piece may go a line length beyond the limit, and if even that doesn't fit, the line is ended with
   "\r\n" at once (always room for that), so the next line still starts clean. */
static void commitLine(LineBuf* lb)
{
   uint32_t old = maskLog();
   uint8_t ends = lb->len > 0 && lb->buf[lb->len - 1] == '\n';
   /* a main-loop line longer than LINE_LEN is wrapped (see above) */
   uint8_t wrap = !ends && lb == &mainLine;
   /* A wrapped piece that ends with the line's own "\r" gets just "\n", and the "\n" that follows is dropped, so the
      line doesn't end in "\r\r\n" followed by an empty "\n" line. */
   uint8_t endsCr = wrap && lb->len > 0 && lb->buf[lb->len - 1] == '\r';
   /* the first piece of a line leaves room for a line length more, so that an interrupt's long line can go on */
   uint16_t firstLimit = RING_LIMIT - LINE_LEN;

   lb->dropLf = endsCr;

   if (lb->broken)
   {
      /* the rest of a line that couldn't go out: discarded up to its end */
      if (ends)
         lb->broken = 0;
   }
   else if (!lb->continued)
   {
      if (ringWrite(lb->buf, lb->len, (uint16_t)(firstLimit - (wrap ? 2 : 0))))
      {
         if (wrap)
            ringWrite(endsCr ? "\n" : "\r\n", endsCr ? 1 : 2, LOG_RING - 1); /* fits: kept free above */
         lb->continued = !ends && !wrap;
      }
      else
      {
         countDrop();
         lb->broken = !ends && !wrap;
      }
   }
   else
   {
      if (ringWrite(lb->buf, lb->len, RING_LIMIT))
      {
         lb->continued = !ends;
      }
      else
      {
         countDrop();
         ringWrite("\r\n", 2, LOG_RING - 1);
         lb->continued = 0;
         lb->broken = !ends;
      }
   }
   startDma();
   lb->len = 0;
   unmaskLog(old);
}

RingTerminal::RingTerminal(uint32_t usart, const TERM_CMD* commands)
: Terminal(usart, commands)
{
   /* The Terminal has set up DMA2 channel 5 for UART4 TX; this takes it over, with its interrupt. */
   nvic_set_priority(NVIC_DMA2_CHANNEL4_5_IRQ, LOG_MASK_LEVEL);
   nvic_enable_irq(NVIC_DMA2_CHANNEL4_5_IRQ);
}

void RingTerminal::PutChar(char c)
{
   LineBuf* lb = currentIpsr() == 0 ? &mainLine : &isrLine;

   if (lb->dropLf)
   {
      lb->dropLf = 0;
      if (c == '\n')
         return;
   }
   lb->buf[lb->len++] = c;
   if (c == '\n' || lb->len == LINE_LEN)
      commitLine(lb);
}

/* Appends text and returns the new end. */
static char* putText(char* p, const char* text)
{
   while (*text)
      *p++ = *text++;
   return p;
}

/* Appends v in decimal and returns the new end. */
static char* putUnsigned(char* p, uint32_t v)
{
   char digits[10];
   int n = 0;

   do
   {
      digits[n++] = (char)('0' + v % 10);
      v /= 10;
   } while (v != 0);
   while (n > 0)
      *p++ = digits[--n];
   return p;
}

/* Once a second, if anything was dropped: "[<ms>] Serial output: <n> lines dropped (ring full)". Formatted here rather
   than by printf, to keep printf off the scheduler's deepest call chain. If it doesn't fit either, it waits for the
   next second. */
static void reportDrops(void)
{
   char line[64]; /* "[4294967295] Serial output: 65535 lines dropped (ring full)\r\n" is 61 */
   char* p = line;
   uint32_t old;

   if (droppedPending == 0)
      return;
   *p++ = '[';
   p = putUnsigned(p, rtc_get_ms());
   p = putText(p, "] Serial output: ");
   p = putUnsigned(p, droppedPending);
   p = putText(p, " lines dropped (ring full)\r\n");
   old = maskLog();
   if (ringWrite(line, (uint16_t)(p - line), RING_LIMIT))
   {
      droppedPending = 0;
      startDma();
   }
   unmaskLog(old);
}

void serialRing_cyclic100ms(void)
{
   if (++dropReportCount >= 10)
   {
      dropReportCount = 0;
      reportDrops();
   }
}

uint32_t serialRing_droppedLines(void)
{
   return droppedTotal;
}
