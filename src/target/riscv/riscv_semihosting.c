// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2018 by Liviu Ionescu                                   *
 *   ilg@livius.net                                                        *
 *                                                                         *
 *   Copyright (C) 2009 by Marvell Technology Group Ltd.                   *
 *   Written by Nicolas Pitre <nico@marvell.com>                           *
 *                                                                         *
 *   Copyright (C) 2010 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2016 by Square, Inc.                                    *
 *   Steven Stallion <stallion@squareup.com>                               *
 ***************************************************************************/

/**
 * @file
 * Hold RISC-V semihosting support.
 *
 * The RISC-V code is inspired from ARM semihosting.
 *
 * Details can be found in chapter 8 of DUI0203I_rvct_developer_guide.pdf
 * from ARM Ltd.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/log.h>

#include "target/target.h"
#include "riscv.h"

static int riscv_semihosting_setup(struct target *target, int enable);
static int riscv_semihosting_post_result(struct target *target);

/**
 * Initialize RISC-V semihosting. Use common ARM code.
 */
void riscv_semihosting_init(struct target *target)
{
	semihosting_common_init(target, riscv_semihosting_setup,
		riscv_semihosting_post_result);
}

/**
 * Check for and process a semihosting request using the ARM protocol). This
 * is meant to be called when the target is stopped due to a debug mode entry.
 *
 * @param target Pointer to the target to process.
 * @param retval Pointer to a location where the return code will be stored
 * @return non-zero value if a request was processed or an error encountered
 */
enum semihosting_result riscv_semihosting(struct target *target, int *retval)
{
	struct semihosting *semihosting = target->semihosting;
	if (!semihosting) {
		LOG_DEBUG("   -> NONE (!semihosting)");
		return SEMIHOSTING_NONE;
	}

	if (!semihosting->is_active) {
		LOG_DEBUG("   -> NONE (!semihosting->is_active)");
		return SEMIHOSTING_NONE;
	}

	riscv_reg_t pc;
	int result = riscv_get_register(target, &pc, GDB_REGNO_PC);
	if (result != ERROR_OK)
		return SEMIHOSTING_ERROR;

	uint8_t tmp_buf[12];

	/* Read three uncompressed instructions: The previous, the current one (pointed to by PC) and the next one */
	for (int i = 0; i < 3; i++) {
		/* Instruction memories may not support arbitrary read size. Use any size that will work. */
		*retval = riscv_read_by_any_size(target, (pc - 4) + 4 * i, 4, tmp_buf + 4 * i);
		if (*retval != ERROR_OK)
			return SEMIHOSTING_ERROR;
	}

	/*
	 * The instructions that trigger a semihosting call,
	 * always uncompressed, should look like:
	 *
	 * 01f01013              slli    zero,zero,0x1f
	 * 00100073              ebreak
	 * 40705013              srai    zero,zero,0x7
	 */
	uint32_t pre = target_buffer_get_u32(target, tmp_buf);
	uint32_t ebreak = target_buffer_get_u32(target, tmp_buf + 4);
	uint32_t post = target_buffer_get_u32(target, tmp_buf + 8);
	LOG_DEBUG("check %08x %08x %08x from 0x%" PRIx64 "-4", pre, ebreak, post, pc);

	if (pre != 0x01f01013 || ebreak != 0x00100073 || post != 0x40705013) {
		/* Not the magic sequence defining semihosting. */
		LOG_DEBUG("   -> NONE (no magic)");
		return SEMIHOSTING_NONE;
	}

	/*
	 * Perform semihosting call if we are not waiting on a fileio
	 * operation to complete.
	 */
	if (!semihosting->hit_fileio) {
		/* RISC-V uses A0 and A1 to pass function arguments */
		riscv_reg_t r0;
		riscv_reg_t r1;

		result = riscv_get_register(target, &r0, GDB_REGNO_A0);
		if (result != ERROR_OK) {
			LOG_DEBUG("   -> ERROR (couldn't read a0)");
			return SEMIHOSTING_ERROR;
		}

		result = riscv_get_register(target, &r1, GDB_REGNO_A1);
		if (result != ERROR_OK) {
			LOG_DEBUG("   -> ERROR (couldn't read a1)");
			return SEMIHOSTING_ERROR;
		}

		semihosting->op = r0;
		semihosting->param = r1;
		semihosting->word_size_bytes = riscv_xlen(target) / 8;

		/* Check for ARM operation numbers. */
		if ((semihosting->op >= 0 && semihosting->op <= 0x31) ||
			(semihosting->op >= 0x100 && semihosting->op <= 0x115)) {

			*retval = semihosting_common(target);
			if (*retval != ERROR_OK) {
				LOG_ERROR("Failed semihosting operation (0x%02X)", semihosting->op);
				return SEMIHOSTING_ERROR;
			}
		} else {
			/* Unknown operation number, not a semihosting call. */
			LOG_DEBUG("   -> NONE (unknown operation number)");
			return SEMIHOSTING_NONE;
		}
	}

	/*
	 * Resume target if we are not waiting on a fileio
	 * operation to complete.
	 */
	if (semihosting->is_resumable && !semihosting->hit_fileio) {
		/* Resume right after the EBREAK 4 bytes instruction. */
		*retval = riscv_set_register(target, GDB_REGNO_PC, pc + 4);
		if (*retval != ERROR_OK)
			return SEMIHOSTING_ERROR;

		LOG_DEBUG("   -> HANDLED");
		return SEMIHOSTING_HANDLED;
	}

	LOG_DEBUG("   -> WAITING");
	return SEMIHOSTING_WAITING;
}

/* -------------------------------------------------------------------------
 * Local functions. */

/**
 * Called via semihosting->setup() later, after the target is known,
 * usually on the first semihosting command.
 */
static int riscv_semihosting_setup(struct target *target, int enable)
{
	LOG_DEBUG("[%s] enable=%d", target_name(target), enable);

	struct semihosting *semihosting = target->semihosting;
	if (semihosting)
		semihosting->setup_time = clock();

	return ERROR_OK;
}

static int riscv_semihosting_post_result(struct target *target)
{
	struct semihosting *semihosting = target->semihosting;
	if (!semihosting) {
		/* If not enabled, silently ignored. */
		return 0;
	}

	LOG_DEBUG("0x%" PRIx64, semihosting->result);
	riscv_set_register(target, GDB_REGNO_A0, semihosting->result);
	return 0;
}
