#include "diagnostics.h"

#include "common.h"
#include "processes.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace {

std::unique_ptr<FILE, decltype(&std::fclose)> gDiagnosticFile{nullptr, &std::fclose};

void logMessage(bool debug, const char *format, va_list arguments) {
	const int savedError = errno;
	FILE *stream = gDiagnosticFile ? gDiagnosticFile.get() : stderr;
	flockfile(stream);
	if (debug) {
		for (unsigned index = 0; index < wibo::debugIndent; ++index)
			std::fputc('\t', stream);
		std::fprintf(stream, "[thread %x] ", wibo::getThreadId());
	}
	std::vfprintf(stream, format, arguments);
	std::fflush(stream);
	funlockfile(stream);
	errno = savedError;
}

} // namespace

void wibo::initializeDiagnostics() {
	const int savedError = errno;
	const char *path = std::getenv("WIBO_DIAGNOSTIC_LOG");
	if (!path || !*path)
		return;
	int descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (descriptor >= 0 && descriptor < 3) {
		// Opening the log must not replace a closed standard descriptor.
		const int independent = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
		const int error = errno;
		close(descriptor);
		descriptor = independent;
		errno = error;
	}
	FILE *stream = descriptor < 0 ? nullptr : fdopen(descriptor, "a");
	if (!stream) {
		const int error = errno;
		if (descriptor >= 0)
			close(descriptor);
		std::fprintf(stderr, "wibo: cannot open diagnostic log '%s': %s; using stderr\n", path, std::strerror(error));
		std::fflush(stderr);
	} else {
		gDiagnosticFile.reset(stream);
	}
	errno = savedError;
}

void wibo::diagnosticLog(const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	logMessage(false, format, arguments);
	va_end(arguments);
}

void wibo::debug_log(const char *fmt, ...) {
	if (!wibo::debugEnabled)
		return;
	va_list arguments;
	va_start(arguments, fmt);
	logMessage(true, fmt, arguments);
	va_end(arguments);
}
