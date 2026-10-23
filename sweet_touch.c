/*
 * sweet_touch.c — Novatek NT36675 SPI touch for Xiaomi Mi 10T 5G (apollo)
 *
 * Drop-in replacement for the original Goodix GT9896 I²C driver.
 * Same public API: sweet_touch_init(), boot-arg sweettouch=, calls sweet_hid_touch().
 *
 * Hardware (from apollo live.dtb + Xiaomi SM8250 trees):
 *   compatible     = "novatek,NVT-ts-spi"
 *   SPI controller = spi@994000 (qcom,spi-geni)
 *   RST GPIO       = TLMM 8
 *   IRQ GPIO       = TLMM 9
 *   Panel          = 1080 × 2400
 *   Firmware       = novatek_nt36675_j3s_fw01.bin (already running from ABL)
 *
 * Strategy (same as original sweet_touch.c):
 *   - One kernel thread, polled + optional IRQ edge
 *   - No firmware upload, no RPMh votes, no PMIC writes
 *   - Only read reports and feed sweet_hid_touch()
 *
 * Status:
 *   GENI SPI programming is a skeleton that mirrors the I2C path.
 *   Once SPI transfers work, the report parser below is ready.
 *   First bring-up goal: get any SPI read and see raw bytes in the log.
 */

#if defined(ARM64_BOARD_CONFIG_SWEET)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);
void sweet_touch_init(void);

/* ---------- apollo hardware ---------- */
#define TOUCH_SE_PA          0x00994000u   /* spi@994000, qcom,spi-geni */
#define TOUCH_GCC_PA         0x00100000u
#define TOUCH_TLMM_PA        0x0f100000u   /* TLMM base (SM8250) */
#define TOUCH_SE_SIZE        0x4000
#define TOUCH_TLMM_SIZE      0x300000

#define TOUCH_RST_GPIO       8
#define TOUCH_IRQ_GPIO       9

#define TOUCH_MAX_FINGERS    10
#define TOUCH_ABS_X          1080
#define TOUCH_ABS_Y          2400

/* Novatek point report size (common for NT36675 SPI) */
#define NVT_POINT_DATA_LEN   67
#define NVT_MAX_POINTS       10

/* GENI SE register offsets (same family as the I2C driver) */
#define SE_GENI_STATUS       0x040
#define SE_GENI_M_CMD0       0x600
#define SE_GENI_M_CMD_CTRL   0x604
#define SE_GENI_M_IRQ_STATUS 0x610
#define SE_GENI_M_IRQ_EN     0x614
#define SE_GENI_M_IRQ_CLEAR  0x618
#define SE_GENI_TX_FIFO      0x700
#define SE_GENI_RX_FIFO      0x780
#define SE_GENI_TX_FIFO_STATUS 0x800
#define SE_GENI_RX_FIFO_STATUS 0x804
#define SE_GENI_TX_WATERMARK 0x80c
#define SE_GENI_RX_WATERMARK 0x810
#define SE_GENI_IOS          0x908
#define SE_GENI_DMA_MODE_EN  0x258
#define SE_GENI_TX_TRANS_LEN 0x26c
#define SE_GENI_RX_TRANS_LEN 0x270
#define SE_GENI_CLK_SEL      0x07c
#define SE_GENI_CFG_SEQ_START 0x48

/* SPI command opcodes used by GENI firmware (typical) */
#define GENI_SPI_TX_ONLY     0x1
#define GENI_SPI_RX_ONLY     0x2
#define GENI_SPI_FULL_DUPLEX 0x3
#define GENI_SPI_TX_RX       0x7   /* common for many SE firmwares */

/* TLMM helpers */
#define TLMM_GPIO_CFG(n)     ((uint32_t)(n) * 0x1000u)
#define TLMM_GPIO_IN_OUT(n)  (TLMM_GPIO_CFG(n) + 4u)

static struct {
	vm_offset_t se;
	vm_offset_t tlmm;
	vm_offset_t gcc;
	bool started;
	bool dead;
	bool debug;
	uint32_t active;                 /* bitmask of currently down fingers */
	int x[TOUCH_MAX_FINGERS];
	int y[TOUCH_MAX_FINGERS];
} st;

static int touch_sleep_chan;

/* ---------- low-level MMIO ---------- */
static uint32_t
touch_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
touch_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
touch_wait_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&touch_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
touch_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (!(touch_rd(base, off) & mask))
			return true;
		delay(1);
	}
	return false;
}

/* ---------- TLMM GPIO ---------- */
static void
touch_gpio_cfg(unsigned pin, unsigned pull, unsigned drive, bool out)
{
	uint32_t off = TLMM_GPIO_CFG(pin);
	uint32_t cfg = touch_rd(st.tlmm, off);

	if (out) {
		/* set output high before enabling output to avoid glitch */
		uint32_t io = touch_rd(st.tlmm, TLMM_GPIO_IN_OUT(pin));
		touch_wr(st.tlmm, TLMM_GPIO_IN_OUT(pin), io | 2u);
	}

	cfg &= ~0x3ffu;
	cfg |= (0u << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	touch_wr(st.tlmm, off, cfg);
}

static void
touch_gpio_set(unsigned pin, int val)
{
	uint32_t io = touch_rd(st.tlmm, TLMM_GPIO_IN_OUT(pin));
	if (val)
		io |= 2u;
	else
		io &= ~2u;
	touch_wr(st.tlmm, TLMM_GPIO_IN_OUT(pin), io);
}

static int
touch_gpio_get(unsigned pin)
{
	return !!(touch_rd(st.tlmm, TLMM_GPIO_IN_OUT(pin)) & 1u);
}

static void
touch_reset_pulse(void)
{
	/* Active-low reset, typical Novatek sequence */
	touch_gpio_cfg(TOUCH_RST_GPIO, 0 /* no pull */, 2 /* 8 mA */, true);
	touch_gpio_set(TOUCH_RST_GPIO, 0);
	touch_wait_ms(10);
	touch_gpio_set(TOUCH_RST_GPIO, 1);
	touch_wait_ms(50);   /* let FW boot */
	printf("SWEET TOUCH: reset pulse done (gpio %u)\n", TOUCH_RST_GPIO);
}

/* ---------- GENI SPI (skeleton matching I2C style) ---------- */
/*
 * The original Goodix driver programmed SE7 for I2C.
 * Here we target the SPI SE at 0x994000.
 *
 * ABL already left the SE in a usable state on most builds; we try the
 * lightest possible path first (assume firmware + clocks already good).
 * If transfers fail we log and mark the driver dead.
 */

static bool
touch_spi_init(void)
{
	uint32_t status = touch_rd(st.se, SE_GENI_STATUS);
	printf("SWEET TOUCH: SE status=%#x @ 0x%x\n", status, TOUCH_SE_PA);

	/* Disable DMA mode — we use FIFO like the I2C path */
	touch_wr(st.se, SE_GENI_DMA_MODE_EN, 0);

	/* Clear any pending IRQs */
	touch_wr(st.se, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);

	/* Very basic: leave firmware configuration alone.
	 * If ABL already loaded SPI firmware this is enough for first tests.
	 */
	return true;
}

/*
 * Minimal SPI write-then-read.
 * Many Novatek firmwares expect:
 *   1) optional page-select (0xff, addr[23:16], addr[15:8])
 *   2) then a single-byte address + data
 * For the continuous point report we often just do a pure RX of N bytes
 * after the IRQ, depending on the exact firmware.
 *
 * This implementation is deliberately simple so it can be iterated on
 * once the first bytes appear in the log.
 */
static int
touch_spi_xfer(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
	unsigned i;

	if (st.dead)
		return -1;

	/* Abort any previous command */
	touch_wr(st.se, SE_GENI_M_CMD_CTRL, 2);
	touch_wait_clear(st.se, SE_GENI_STATUS, 1u);

	/* Program lengths */
	if (tx_len)
		touch_wr(st.se, SE_GENI_TX_TRANS_LEN, (uint32_t)tx_len);
	if (rx_len)
		touch_wr(st.se, SE_GENI_RX_TRANS_LEN, (uint32_t)rx_len);

	/* Push TX data into FIFO (4 bytes at a time when possible) */
	for (i = 0; i < tx_len; ) {
		uint32_t word = 0;
		unsigned n = (tx_len - i) > 4 ? 4 : (unsigned)(tx_len - i);
		for (unsigned b = 0; b < n; ++b)
			word |= (uint32_t)tx[i + b] << (8 * b);
		touch_wr(st.se, SE_GENI_TX_FIFO, word);
		i += n;
	}

	/* Start command — opcode may need tuning for the exact SE firmware */
	uint32_t cmd = (rx_len && tx_len) ? GENI_SPI_FULL_DUPLEX :
	               (rx_len ? GENI_SPI_RX_ONLY : GENI_SPI_TX_ONLY);
	touch_wr(st.se, SE_GENI_M_CMD0, cmd | ((uint32_t)(tx_len + rx_len) << 4));

	/* Wait for command done (bit 0 of IRQ status is typical) */
	bool done = false;
	for (i = 0; i < 10000; ++i) {
		uint32_t irq = touch_rd(st.se, SE_GENI_M_IRQ_STATUS);
		if (irq & 1u) {
			done = true;
			touch_wr(st.se, SE_GENI_M_IRQ_CLEAR, irq);
			break;
		}
		if (irq & 0x0eu) { /* error bits */
			printf("SWEET TOUCH: SPI error irq=%#x\n", irq);
			touch_wr(st.se, SE_GENI_M_IRQ_CLEAR, irq);
			st.dead = true;
			return -1;
		}
		delay(1);
	}
	if (!done) {
		printf("SWEET TOUCH: SPI timeout\n");
		st.dead = true;
		return -1;
	}

	/* Pull RX data */
	for (i = 0; i < rx_len; ) {
		uint32_t word = touch_rd(st.se, SE_GENI_RX_FIFO);
		unsigned n = (rx_len - i) > 4 ? 4 : (unsigned)(rx_len - i);
		for (unsigned b = 0; b < n; ++b)
			rx[i + b] = (uint8_t)(word >> (8 * b));
		i += n;
	}

	return 0;
}

/* Convenience: pure read of the continuous report buffer */
static int
touch_spi_read_report(uint8_t *buf, size_t len)
{
	/* Many NT36xxx SPI firmwares expose the point report at a fixed
	 * address.  A common pattern is a short address phase then RX.
	 * Start with a pure RX; if that returns zeros, try an address prefix.
	 */
	uint8_t addr[] = { 0x00 };  /* try simple first */
	return touch_spi_xfer(addr, sizeof(addr), buf, len);
}

/* ---------- Novatek report parser ---------- */
/*
 * Common NT36xxx SPI point format (simplified, matches many Xiaomi firmwares):
 *
 *   byte 0 : input event / finger count (low nibble = number of fingers)
 *   then 6-byte slots (or 8-byte on some FW):
 *     [0]  contact id + status
 *     [1]  x high
 *     [2]  x low
 *     [3]  y high
 *     [4]  y low
 *     [5]  pressure / width
 *
 * Coordinates are usually 12-bit or 16-bit.  We clamp to the panel size.
 * Once you have real dumps, adjust the offsets — the structure is here.
 */
static void
touch_parse_report(const uint8_t *r, size_t len)
{
	if (len < 7)
		return;

	if (st.debug) {
		printf("SWEET TOUCH: raw");
		for (size_t i = 0; i < (len < 24 ? len : 24); ++i)
			printf(" %02x", r[i]);
		printf("\n");
	}

	/* Heuristic: finger count in low nibble of first byte */
	unsigned fingers = r[0] & 0x0fu;
	if (fingers > NVT_MAX_POINTS)
		fingers = NVT_MAX_POINTS;

	uint32_t now = 0;
	unsigned slot = 1;   /* start of first finger after header */

	for (unsigned i = 0; i < fingers && (slot + 5) < len; ++i) {
		uint8_t status = r[slot + 0];
		int id = status & 0x0f;
		int down = (status & 0x40) ? 0 : 1;   /* bit6 often = tip switch inverted on some FW */

		/* Try 12-bit packing first (common) */
		int x = ((r[slot + 1] & 0x0f) << 8) | r[slot + 2];
		int y = ((r[slot + 1] & 0xf0) << 4) | r[slot + 3];

		/* Fallback: 16-bit little-endian if numbers look insane */
		if (x > TOUCH_ABS_X * 2 || y > TOUCH_ABS_Y * 2) {
			x = r[slot + 1] | (r[slot + 2] << 8);
			y = r[slot + 3] | (r[slot + 4] << 8);
		}

		if (x < 0) x = 0;
		if (y < 0) y = 0;
		if (x >= TOUCH_ABS_X) x = TOUCH_ABS_X - 1;
		if (y >= TOUCH_ABS_Y) y = TOUCH_ABS_Y - 1;

		if (id >= TOUCH_MAX_FINGERS)
			id = i;

		if (down) {
			now |= (1u << id);
			if (!(st.active & (1u << id)) ||
			    st.x[id] != x || st.y[id] != y) {
				st.x[id] = x;
				st.y[id] = y;
				sweet_hid_touch(id, 1, x, y);
			}
		}

		slot += 6;   /* advance to next finger; try 8 if needed */
	}

	/* Lift fingers that disappeared */
	for (unsigned id = 0; id < TOUCH_MAX_FINGERS; ++id) {
		if ((st.active & (1u << id)) && !(now & (1u << id))) {
			sweet_hid_touch(id, 0, st.x[id], st.y[id]);
		}
	}
	st.active = now;
}

/* ---------- main thread (same model as original) ---------- */
static void
touch_thread(void *arg, wait_result_t result)
{
	uint8_t report[NVT_POINT_DATA_LEN];
	unsigned idle = 0;
	(void)arg;
	(void)result;

	printf("SWEET TOUCH: thread running (NT36675 SPI)\n");

	for (;;) {
		if (st.dead) {
			touch_wait_ms(1000);
			continue;
		}

		/* Poll the IRQ GPIO.  When it goes active we read a report.
		 * Edge is usually active-low or rising depending on pinctrl;
		 * we just sample and react to transitions.
		 */
		int irq = touch_gpio_get(TOUCH_IRQ_GPIO);

		if (irq == 0 || idle > 5) {   /* active-low or periodic poll */
			if (touch_spi_read_report(report, sizeof(report)) == 0) {
				touch_parse_report(report, sizeof(report));
				idle = 0;
			} else {
				idle++;
			}
		} else {
			idle++;
		}

		/* ~8–10 ms polling is fine for bring-up; can be lowered later */
		touch_wait_ms(10);
	}
}

/* ---------- public entry (identical signature to original) ---------- */
void
sweet_touch_init(void)
{
	int enabled = 1;
	thread_t thread;

	if (st.started)
		return;
	st.started = true;

	(void)PE_parse_boot_argn("sweettouch", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("SWEET TOUCH: disabled (sweettouch=0)\n");
		return;
	}

	(void)PE_parse_boot_argn("sweettouch_debug", &st.debug, sizeof(st.debug));

	st.se   = ml_io_map(TOUCH_SE_PA, TOUCH_SE_SIZE);
	st.tlmm = ml_io_map(TOUCH_TLMM_PA, TOUCH_TLMM_SIZE);
	st.gcc  = ml_io_map(TOUCH_GCC_PA, 0x20000);

	if (!st.se || !st.tlmm) {
		printf("SWEET TOUCH: MMIO map failed (se=%p tlmm=%p)\n",
		    (void *)st.se, (void *)st.tlmm);
		return;
	}

	printf("SWEET TOUCH: apollo NT36675 @ SPI 0x%x  RST=gpio%u IRQ=gpio%u\n",
	    TOUCH_SE_PA, TOUCH_RST_GPIO, TOUCH_IRQ_GPIO);

	/* Configure IRQ as input, pull-up (common) */
	touch_gpio_cfg(TOUCH_IRQ_GPIO, 3 /* pull-up */, 0, false);

	touch_reset_pulse();

	if (!touch_spi_init()) {
		printf("SWEET TOUCH: SPI init failed\n");
		return;
	}

	kern_return_t kr = kernel_thread_start(touch_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
		printf("SWEET TOUCH: started\n");
	} else {
		printf("SWEET TOUCH: kernel_thread_start failed %d\n", kr);
	}
}

#endif /* ARM64_BOARD_CONFIG_SWEET */
