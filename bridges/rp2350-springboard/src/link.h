#pragma once

/* CS1 link (include/sbnio.h) between the Palm and NIO's packet link on USB port
 * 0. */
void link_init(void);
void link_poll(void);
void link_command(const char *line);
