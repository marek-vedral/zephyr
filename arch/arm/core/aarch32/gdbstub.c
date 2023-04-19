#include <zephyr/kernel.h>
#include <kernel_internal.h>
#include <zephyr/arch/arm/aarch32/gdbstub.h>

#define DBGDSCR_MONITOR_MODE_EN 0x8000

#define DBGDBCR_ADDR_MASK_MASK		0x1F
#define DBGDBCR_ADDR_MASK_SHIFT		24
#define DBGDBCR_MEANING_MASK		0x7
#define DBGDBCR_MEANING_SHIFT		20
#define DBGDBCR_MEANING_ADDR_MATCH	0x0
#define DBGDBCR_LINKED_BRP_MASK		0xF
#define DBGDBCR_LINKED_BRP_SHIFT	16
#define DBGDBCR_SECURE_STATE_MASK	0x3
#define DBGDBCR_SECURE_STATE_SHIFT	14
#define DBGDBCR_BYTE_ADDR_MASK		0xF
#define DBGDBCR_BYTE_ADDR_SHIFT		5
#define DBGDBCR_SUPERVISOR_ACCESS_MASK	0x3
#define DBGDBCR_SUPERVISOR_ACCESS_SHIFT 1
#define DBGDBCR_BRK_EN_MASK		0x1

#define SPSR_REG_IDX 25

/* Position of each register in the packet, see GDB code */
static const int packet_pos[] = {0, 1, 2, 3, 12, 14, 15, 41};

/* Required struct */
static struct gdb_ctx ctx;

static int first_entry;

/* Wrapper function to save and restore execution context */
void z_gdb_entry(z_arch_esf_t *esf, unsigned int exc_cause)
{
	/* Disable the hardware breapoint in case it was set */
	__asm__ volatile("mcr p14, 0, %0, c0, c0, 5" ::"r"(0x0) :);

	// TODO add more exception causes - the stub supports just the debug event (0x2)
	ctx.exception = exc_cause;
	// save the registers
	ctx.registers[R0] = esf->basic.r0;
	ctx.registers[R1] = esf->basic.r1;
	ctx.registers[R2] = esf->basic.r2;
	ctx.registers[R3] = esf->basic.r3;
	ctx.registers[R12] = esf->basic.r12;
	ctx.registers[LR] = esf->basic.lr;
	ctx.registers[PC] = esf->basic.pc;
	ctx.registers[SPSR] = esf->basic.xpsr;

	z_gdb_main_loop(&ctx);

	esf->basic.r0 = ctx.registers[R0];
	esf->basic.r1 = ctx.registers[R1];
	esf->basic.r2 = ctx.registers[R2];
	esf->basic.r3 = ctx.registers[R3];
	esf->basic.r12 = ctx.registers[R12];
	esf->basic.lr = ctx.registers[LR];
	if (first_entry) {
		/* The CPU should continue on the next instruction - apply this offset,
		so that it won't be affected by the bkpt instruction */
		esf->basic.pc = ctx.registers[PC] + 0x4;
	} else {
		esf->basic.pc = ctx.registers[PC];
	}
	first_entry = 0;
	esf->basic.xpsr = ctx.registers[SPSR];
}

void arch_gdb_init(void)
{
	/* Enable the monitor debug mode */
	first_entry = 1;
	uint32_t reg_val;
	/* Enable the monitor debug mode */
	__asm__ volatile("mrc p14, 0, %0, c0, c2, 2" : "=r"(reg_val)::);
	reg_val |= DBGDSCR_MONITOR_MODE_EN;
	__asm__ volatile("mcr p14, 0, %0, c0, c2, 2" ::"r"(reg_val) :);

	/* Generate the Prefetch abort exception */
	__asm__ volatile("BKPT");
}

void arch_gdb_continue(void)
{
	/* No need to do anything, return to the code. */
}

void arch_gdb_step(void)
{
	/* Set the hardware breakpoint TODO */
	uint32_t reg_val = ctx.registers[PC];
	/* set BVR to PC, make sure it is word aligned */
	reg_val &= ~(0x3);
	__asm__ volatile("mcr p14, 0, %0, c0, c0, 4" ::"r"(reg_val) :);

	/* Program the BCR for address mismatch */
	reg_val = 0;
	/* no linked BRP, match in both secure and non-secure state, match all bytes */
	reg_val |= (0x4 & DBGDBCR_MEANING_MASK) << DBGDBCR_MEANING_SHIFT;
	reg_val |= (0xF & DBGDBCR_BYTE_ADDR_MASK) << DBGDBCR_BYTE_ADDR_SHIFT;
	/* Active only in system, user and supervisor modes */
	reg_val |= (0x0 & DBGDBCR_SUPERVISOR_ACCESS_MASK) << DBGDBCR_SUPERVISOR_ACCESS_SHIFT;
	/* Breakpoint enable */
	reg_val |= DBGDBCR_BRK_EN_MASK;
	__asm__ volatile("mcr p14, 0, %0, c0, c0, 5" ::"r"(reg_val) :);
}

size_t arch_gdb_reg_readall(struct gdb_ctx *ctx, uint8_t *buf, size_t buflen)
{
	int ret = 0;
	/* Write cached registers to the buf array */
	memset(buf, 'x', buflen);
	for (int i = 0; i < GDB_STUB_NUM_REGISTERS; i++) {
		/* offset inside the packet */
		int pos = packet_pos[i] * 8;
		int r = bin2hex((const uint8_t *)(ctx->registers + i), 4, buf + pos, buflen - pos);
		/* remove the newline character */
		buf[pos + 8] = 'x';
		if (r == 0) {
			ret = 0;
			break;
		}
		ret += r;
	}

	if (ret) {
		/* since we don't support some floating point registers, set the packet size
		 * manually */
		ret = 41 * 8 + 8;
	}
	return ret;
}

size_t arch_gdb_reg_writeall(struct gdb_ctx *ctx, uint8_t *hex, size_t hexlen)
{
	int ret = 0;
	for (unsigned int i = 0; i < hexlen; i += 8) {
		if (hex[i] != 'x') {
			/* check if the stub supports this register */
			for (unsigned int j = 0; j < GDB_STUB_NUM_REGISTERS; j++) {
				if (packet_pos[j] != i) {
					continue;
				}
				int r = hex2bin(hex + i * 8, 8, (uint8_t *)(ctx->registers + j), 4);
				if (r == 0) {
					return 0;
				}
				ret += r;
			}
		}
	}
	return ret;
}

size_t arch_gdb_reg_readone(struct gdb_ctx *ctx, uint8_t *buf, size_t buflen, uint32_t regno)
{
	int ret = 0;
	/* Fill the buffer with 'x' in case the stub does not support the required register */
	memset(buf, "xxxxxxxx", 8);
	ret = 8;
	if (regno == SPSR_REG_IDX) {
		/* The SPSR register is at the end, we have to check separately */
		ret = bin2hex((uint8_t *)(ctx->registers + GDB_STUB_NUM_REGISTERS - 1), 4, buf,
			      buflen);
	} else {
		/* Check which of our registers corresponds to regnum */
		for (int i = 0; i < GDB_STUB_NUM_REGISTERS; i++) {
			if (packet_pos[i] == regno) {
				ret = bin2hex((uint8_t *)(ctx->registers + i), 4, buf, buflen);
				break;
			}
		}
	}
	return ret;
}

size_t arch_gdb_reg_writeone(struct gdb_ctx *ctx, uint8_t *hex, size_t hexlen, uint32_t regno)
{
	int ret = 0;
	/* Set the value of a register */
	if (hexlen != 8) {
		return ret;
	}

	if (regno < (GDB_STUB_NUM_REGISTERS - 1)) {
		/* Again, check the corresponding register index */
		for (int i = 0; i < GDB_STUB_NUM_REGISTERS; i++) {
			if (packet_pos[i] == regno) {
				ret = hex2bin(hex, hexlen, (uint8_t *)(ctx->registers + i), 4);
				break;
			}
		}
	} else if (regno == SPSR_REG_IDX) {
		ret = hex2bin(hex, hexlen, (uint8_t *)(ctx->registers + GDB_STUB_NUM_REGISTERS - 1),
			      4);
	}
	return ret;
}
