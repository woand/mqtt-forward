#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#define SERVER_SIDE_PREFIX "SERVER-SIDE: "
#define CLIENT_SIDE_PREFIX "CLIENT-SIDE: "

bool server_mode;
static int mqtt_forward_log_level = LOG_INFO;

void mqtt_forward_set_log_level(int level)
{
	mqtt_forward_log_level = level;
}

int mqtt_forward_get_log_level(void)
{
	return mqtt_forward_log_level;
}

void mqtt_forward_log(int level, const char *fmt, ...)
{
	va_list args;

	if (level > mqtt_forward_log_level)
		return;

	(void)fputs(server_mode ? SERVER_SIDE_PREFIX : CLIENT_SIDE_PREFIX,
		    stderr);
	va_start(args, fmt);
	(void)vfprintf(stderr, fmt, args);
	va_end(args);
}
