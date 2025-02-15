/* SPDX-License-Identifier: GPL-2.0-or-later */

/***************************************************************************
 *   Module to run arbitrary code on RISCV using OpenOCD                   *
 *   Copyright (C) 2021 Espressif Systems Ltd.                             *
 ***************************************************************************/

#ifndef OPENOCD_TARGET_ESP_RISCV_ALGO_H
#define OPENOCD_TARGET_ESP_RISCV_ALGO_H

#include <target/espressif/esp_algorithm.h>
#include <target/riscv/riscv.h>

/** Index of the first user-defined algo arg. @see algorithm_stub */
#define ESP_RISCV_STUB_ARGS_FUNC_START  2

/**
 * RISCV algorithm data.
 */
struct esp_riscv_algorithm {
	/** Registers with numbers below and including this number will be backuped before algo start.
	 * Set to GDB_REGNO_COUNT-1 to save all existing registers. @see enum gdb_regno. */
	size_t max_saved_reg;
	uint64_t saved_registers[RISCV_MAX_REGISTERS];
	bool valid_saved_registers[RISCV_MAX_REGISTERS];
};

extern const struct algorithm_hw riscv_algo_hw;

#endif	/* OPENOCD_TARGET_ESP_RISCV_ALGO_H */
