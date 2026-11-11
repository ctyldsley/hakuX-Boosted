#include <stdarg.h>
#include "hook_impl.h"

__attribute__((visibility("default"))) int ioctl(int fd, int request, ...) {
	va_list args;
	va_start(args, request);
	void *arg = va_arg(args, void *);
	va_end(args);
	return hook_ioctl(fd, request, arg);
}
