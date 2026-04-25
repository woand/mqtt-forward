#include "log.h"

#include <stdarg.h>
#include <stdio.h>

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

	va_start(args, fmt);
	(void)vfprintf(stderr, fmt, args);
	va_end(args);
}
