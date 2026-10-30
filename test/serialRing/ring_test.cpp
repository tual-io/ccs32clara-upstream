/* Host test of the non-blocking serial output (ccs/serialRing.cpp). Runs on a PC: make -C test/serialRing run

   ccs/serialRing.cpp is compiled as it is, with the hardware simulated (serialRing_hosttest.h):
   - DMA2 channel 5 takes bytes from the ring at a random pace (0 to a few bytes per step), as the UART does, from the
     address and count the code last programmed. Writing the address or count while the channel is enabled fails the
     test (the STM32 ignores it). The transfer-complete flag is set when the count reaches 0.
   - Its interrupt runs whenever the flag and the interrupt enable are set and BASEPRI doesn't hold it off: between
     any two steps of the code outside a masked section, and at once when a masked section ends.
   - The main loop prints lines one character at a time; the scheduler's interrupt (which the main loop can't
     interrupt) prints whole lines and runs the 100 ms task between any two of those characters.

   Checks, over millions of writes and thousands of trips round the ring:
   - Byte for byte: what reached the UART is exactly what was put into the ring, in the same order, each byte once.
     (What was put in is read from the ring right after each write, before anything else can touch it.)
   - No interrupt's output lands inside the main loop's line while it is being put into the ring.
   - The drop count: every line printed either reached the UART whole or was counted as dropped, and the "lines
     dropped" lines add up to the count.

   usage: ring_test [seed] [operations] [long] */

#include "../../ccs/serialRing.cpp"

#include <random>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

/* --- the simulation --- */

static std::mt19937 rng(1);
static uint32_t simBasepri;
static uint32_t simIpsr;
static bool simInIsr;
static uint32_t simTicks;
static int uartMax = 3; /* bytes the UART may take per step */

static struct
{
   bool en, tcie, tcif;
   uint32_t addr;
   uint16_t ndtr;
} dma;

static std::string wire;     /* what the UART sent */
static std::string expected; /* what was put into the ring */
static unsigned long failures;

/* coverage */
static unsigned long isrRuns, isrHeldOff, segmentsAtWrap, writesEndingAtWrap, dmaStarts;

static void fail(const char* what)
{
   if (failures++ < 10)
      printf("FAIL: %s\n", what);
}

static bool isrAllowed(void)
{
   return dma.tcif && dma.tcie && !simInIsr && (simBasepri == 0 || simBasepri > LOG_MASK_LEVEL);
}

static void maybeIsr(void)
{
   if (dma.tcif && dma.tcie && !simInIsr && !isrAllowed())
      isrHeldOff++;
   if (isrAllowed())
   {
      uint32_t savedIpsr = simIpsr;
      simInIsr = true;
      simIpsr = 16 + NVIC_DMA2_CHANNEL4_5_IRQ;
      isrRuns++;
      dma2_channel4_5_isr();
      simIpsr = savedIpsr;
      simInIsr = false;
   }
}

/* The UART takes a few bytes; then the interrupt runs if it may. */
static void hwStep(void)
{
   int k = (int)(rng() % (unsigned)(uartMax + 1));
   uint32_t base = (uint32_t)(uintptr_t)ring;

   simTicks += 1 + rng() % 40;
   while (k-- > 0 && dma.en && dma.ndtr > 0)
   {
      uint32_t off = dma.addr - base;
      if (off >= LOG_RING)
      {
         fail("the DMA reads outside the ring");
         dma.en = false;
         break;
      }
      wire += (char)ring[off];
      dma.addr++;
      if (--dma.ndtr == 0)
      {
         dma.tcif = true;
         if (off == LOG_RING - 1)
            segmentsAtWrap++;
      }
   }
   maybeIsr();
}

void dma_disable_channel(uint32_t, uint8_t)
{
   hwStep();
   dma.en = false;
}

void dma_enable_channel(uint32_t, uint8_t)
{
   dma.en = true;
   dmaStarts++;
   hwStep();
}

void dma_set_memory_address(uint32_t, uint8_t, uint32_t address)
{
   if (dma.en)
      fail("DMA address written while the channel is enabled");
   dma.addr = address;
}

void dma_set_number_of_data(uint32_t, uint8_t, uint16_t number)
{
   if (dma.en)
      fail("DMA count written while the channel is enabled");
   if (number == 0)
      fail("DMA started with a count of 0");
   dma.ndtr = number;
}

void dma_clear_interrupt_flags(uint32_t, uint8_t, uint32_t interrupts)
{
   if (interrupts & DMA_TCIF)
      dma.tcif = false;
}

void dma_enable_transfer_complete_interrupt(uint32_t, uint8_t)
{
   dma.tcie = true;
}

bool dma_get_interrupt_flag(uint32_t, uint8_t, uint32_t flags)
{
   return (flags & DMA_TCIF) && dma.tcif;
}

void nvic_set_priority(uint8_t, uint8_t) {}
void nvic_enable_irq(uint8_t) {}

uint32_t rtc_get_ms(void)
{
   return simTicks / 100;
}

uint32_t maskLog(void)
{
   uint32_t old = simBasepri;
   hwStep();
   if (simBasepri == 0 || simBasepri > LOG_MASK_LEVEL)
      simBasepri = LOG_MASK_LEVEL;
   return old;
}

void unmaskLog(uint32_t old)
{
   simBasepri = old;
   maybeIsr();
   hwStep();
}

uint32_t currentIpsr(void)
{
   return simIpsr;
}

/* --- writing, and recording what was put into the ring --- */

static int captureDepth; /* an interrupt's write inside another write is read with the outer one */

template <class F> static void committed(F f)
{
   if (captureDepth > 0)
   {
      f();
      return;
   }
   captureDepth++;
   uint16_t h0 = head;
   f();
   captureDepth--;
   uint16_t h1 = head;
   for (uint16_t i = h0; i != h1; i = (uint16_t)((i + 1) % LOG_RING))
      expected += (char)ring[i];
   if (h1 == 0 && h0 != 0)
      writesEndingAtWrap++;
}

static RingTerminal* term;
static bool allowLongLines;
static unsigned long preemptions;
static unsigned long linesTried;

static std::string randomText(int n)
{
   static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 =.,:-_/[]()";
   std::string s;
   for (int i = 0; i < n; i++)
      s += chars[rng() % (sizeof(chars) - 1)];
   return s;
}

/* A line; long lines (sent in pieces) only when allowed. */
static std::string randomLine(bool allowLong)
{
   int maxLen = allowLong && rng() % 20 == 0 ? 400 : LINE_LEN - 10;
   return "[" + std::to_string(rng() % 100000) + "] " + randomText(rng() % (maxLen - 12)) + "\r\n";
}

static void putLine(const std::string& s)
{
   for (char c : s)
      committed([&] { term->PutChar(c); });
}

static void isrAction(bool allowLong)
{
   uint32_t saved = simIpsr;
   simIpsr = 16 + 30; /* TIM4 */
   if (rng() % 100 < 80)
   {
      std::string s = randomLine(allowLong);
      linesTried++;
      putLine(s);
   }
   else
   {
      committed([&] { serialRing_cyclic100ms(); });
   }
   simIpsr = saved;
}

/* Before each byte goes into the ring: time passes, and the scheduler's interrupt (priority 0xE0) may interrupt the
   main loop, unless BASEPRI holds it off. Correct code holds it off while it writes, so this only runs the DMA
   interrupt (in hwStep); a missing mask would let the interrupt's output land inside the main loop's. */
void hostPreemptionPoint(void)
{
   hwStep();
   if (simIpsr == 0 && (simBasepri == 0 || simBasepri > 0xE0) && rng() % 32 == 0)
   {
      preemptions++;
      isrAction(allowLongLines);
   }
}

/* --- the run --- */

static int run(unsigned seed, unsigned long ops, bool allowLong)
{
   int isrShare = 40; /* percent of steps the scheduler's interrupt writes something; the rest the UART runs */
   rng.seed(seed);
   std::string pending;
   size_t pendingPos = 0;

   for (unsigned long i = 0; i < ops; i++)
   {
      if (i % 50000 == 0)
      {
         static const int paces[] = { 0, 1, 1, 2, 3, 6, 12 };
         static const int shares[] = { 2, 5, 10, 40 };
         uartMax = paces[rng() % 7];
         isrShare = shares[rng() % 4];
      }
      int r = rng() % 100;
      if (r < 45)
      {
         /* the main loop prints its next character */
         if (pendingPos >= pending.size())
         {
            pending = randomLine(allowLong);
            pendingPos = 0;
            linesTried++;
         }
         char c = pending[pendingPos++];
         simIpsr = 0;
         committed([&] { term->PutChar(c); });
      }
      else if (r < 45 + isrShare)
      {
         isrAction(allowLong);
      }
      else
      {
         hwStep();
      }
   }
   /* finish the main loop's line, then let the UART send everything */
   simIpsr = 0;
   while (pendingPos < pending.size())
   {
      char c = pending[pendingPos++];
      committed([&] { term->PutChar(c); });
   }
   uartMax = 8;
   for (int i = 0; i < 10000000 && (head != tail || dma.ndtr != 0 || dma.tcif); i++)
      hwStep();
   simIpsr = 16 + 30;
   for (int i = 0; i < 10; i++)
      committed([&] { serialRing_cyclic100ms(); }); /* the last drop report */
   simIpsr = 0;
   for (int i = 0; i < 10000000 && (head != tail || dma.ndtr != 0 || dma.tcif); i++)
      hwStep();

   /* byte for byte */
   if (wire != expected)
   {
      size_t k = 0;
      while (k < wire.size() && k < expected.size() && wire[k] == expected[k])
         k++;
      printf("wire %zu bytes, ring %zu bytes, first difference at byte %zu\n", wire.size(), expected.size(), k);
      fail("what reached the UART differs from what was put into the ring");
   }

   /* the lines that reached the UART */
   unsigned long got = 0, reported = 0;
   size_t pos = 0;
   while (pos < wire.size())
   {
      size_t e = wire.find("\r\n", pos);
      if (e == std::string::npos)
      {
         fail("the output ends in the middle of a line");
         break;
      }
      std::string line = wire.substr(pos, e - pos);
      pos = e + 2;
      unsigned n;
      const char* p = strstr(line.c_str(), "] Serial output: ");
      if (p && sscanf(p, "] Serial output: %u lines dropped", &n) == 1)
      {
         reported += n;
         continue;
      }
      got++;
   }

   if (!allowLong && linesTried - got != droppedTotal)
      fail("lines lost differ from the count");
   if (reported + droppedPending != droppedTotal)
      fail("the \"lines dropped\" lines don't add up to the count");
   if (serialRing_droppedLines() != droppedTotal)
      fail("serialRing_droppedLines() differs from the count");
   if (preemptions != 0)
      fail("the scheduler's interrupt could write while the main loop was putting bytes into the ring");

   printf("seed %u, %lu steps%s: %zu bytes out (%zu trips round the ring), lines %lu/%lu, %u dropped\n", seed, ops,
          allowLong ? " with long lines" : "", wire.size(), wire.size() / LOG_RING, got, linesTried,
          (unsigned)droppedTotal);
   printf("  coverage: %lu DMA starts, %lu interrupts, %lu held off while masked, %lu transfers ending at the "
          "ring's end, %lu writes ending at it\n", dmaStarts, isrRuns, isrHeldOff, segmentsAtWrap, writesEndingAtWrap);
   return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
   unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], 0, 10) : 1;
   unsigned long ops = argc > 2 ? strtoul(argv[2], 0, 10) : 3000000;
   bool allowLong = argc > 3 && argv[3][0] == 'l';
   RingTerminal t(0, 0);
   term = &t;
   allowLongLines = allowLong;
   int rc = run(seed, ops, allowLong);
   if (rc == 0)
      printf("OK\n");
   else
      printf("FAILED\n");
   return rc;
}
