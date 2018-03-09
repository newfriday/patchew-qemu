/*
 * SD/MMC cards common
 *
 * Copyright (c) 2018  Philippe Mathieu-Daudé <f4bug@amsat.org>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SD_INTERNAL_H
#define SD_INTERNAL_H

#define SDMMC_CMD_MAX 64

/**
 * sd_cmd_name:
 * @cmd: A SD "normal" command, upto SDMMC_CMD_MAX.
 *
 * Returns an human useful name describing the command.
 *
 * Returns: The command name of @cmd or "UNKNOWN_CMD".
 */
const char *sd_cmd_name(uint8_t cmd);

/**
 * sd_acmd_name:
 * @cmd: A SD "Application-Specific" command, upto SDMMC_CMD_MAX.
 *
 * Returns an human useful name describing the application command.
 *
 * Returns: The application command name of @cmd or "UNKNOWN_ACMD".
 */
const char *sd_acmd_name(uint8_t cmd);

#endif
