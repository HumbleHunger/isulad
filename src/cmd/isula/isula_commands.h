/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2017-2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: lifeng
 * Create: 2017-11-22
 * Description: provide container commands definition
 ******************************************************************************/
#ifndef CMD_ISULA_ISULA_COMMANDS_H
#define CMD_ISULA_ISULA_COMMANDS_H

#include <stdbool.h>

#include "client_arguments.h"

#ifdef __cplusplus
extern "C" {
#endif

// A command is described by:
// @name: The name which should be passed as a second parameter
// @executor: The function that will be executed if the command
// matches. Receives the argc of the program minus two, and
// the rest os argv
// @description: Brief description, will show in help messages
// @longdesc: Long description to show when you run `help <command>`
struct command {
    const char * const name;
    const bool have_subcmd;
    int (*executor)(int, const char **);
    const char * const description;
    const char * const longdesc;
    struct client_arguments *args;
};

// Gets a pointer to a command, to allow implementing custom behavior
// returns null if not found.
// NOTE: Command arrays must end in a command with all member is NULL
const struct command *command_by_name(const struct command *cmds, const char * const name);

int compare_commands(const void *s1, const void *s2);

// Default help command if implementation doesn't prvide one
int command_default_help(const char * const program_name, struct command *commands, int argc, const char **argv);

int command_subcmd_help(const char * const program_name, struct command *commands, int argc, const char **argv);

int run_command(struct command *commands, int argc, const char **argv);

#ifdef __cplusplus
}
#endif

#endif // CMD_ISULA_ISULA_COMMANDS_H
