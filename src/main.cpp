#include "common.h"
#include "diagnostics.h"
#include "entry.h"
#include "entry_trampolines.h"
#include "files.h"
#include "heap.h"
#include "kernel32/heapapi.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "processes.h"
#include "setup.h"
#include "strutil.h"
#include "tls.h"
#include "types.h"
#include "version_info.h"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#if defined(__APPLE__) && defined(WIBO_GUEST_64)
#include <fcntl.h>
#include <sys/ucontext.h>
#endif
#include <unistd.h>

char **wibo::argv;
int wibo::argc;
std::filesystem::path wibo::guestExecutablePath;
std::string wibo::commandLine;
std::vector<uint16_t> wibo::commandLineW;
wibo::ModuleInfo *wibo::mainModule = nullptr;
bool wibo::debugEnabled = false;
unsigned int wibo::debugIndent = 0;
int wibo::tibEntryNumber = -1;
PEB *wibo::processPeb = nullptr;
thread_local TEB *currentThreadTeb = nullptr;

namespace {

#if defined(__APPLE__) && defined(WIBO_GUEST_64)
int gDarwinCrashLogFd = -1;

char *appendText(char *out, const char *end, const char *text) {
	while (out != end && *text != '\0') {
		*out++ = *text++;
	}
	return out;
}

char *appendHex(char *out, const char *end, uintptr_t value) {
	static constexpr char kHex[] = "0123456789abcdef";
	out = appendText(out, end, "0x");
	bool emitted = false;
	for (int shift = static_cast<int>(sizeof(value) * 8) - 4; shift >= 0 && out != end; shift -= 4) {
		const unsigned digit = static_cast<unsigned>((value >> shift) & 0xf);
		if (digit != 0 || emitted || shift == 0) {
			*out++ = kHex[digit];
			emitted = true;
		}
	}
	return out;
}

void recordDarwinFatalSignal(int signalNumber, siginfo_t *info, void *rawContext) {
	if (gDarwinCrashLogFd < 0) {
		return;
	}
	auto *context = static_cast<ucontext_t *>(rawContext);
	const uintptr_t instruction =
		context && context->uc_mcontext ? static_cast<uintptr_t>(context->uc_mcontext->__ss.__rip) : 0;
	const uintptr_t faultAddress = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;

	char buffer[160];
	char *out = buffer;
	const char *end = buffer + sizeof(buffer);
	out = appendText(out, end, "wibo64: pid=");
	out = appendHex(out, end, static_cast<uintptr_t>(getpid()));
	out = appendText(out, end, " signal=");
	out = appendHex(out, end, static_cast<uintptr_t>(signalNumber));
	out = appendText(out, end, " rip=");
	out = appendHex(out, end, instruction);
	out = appendText(out, end, " fault=");
	out = appendHex(out, end, faultAddress);
	if (out != end) {
		*out++ = '\n';
	}
	(void)write(gDarwinCrashLogFd, buffer, static_cast<size_t>(out - buffer));
}

void installDarwinSignalPolicy() {
	// GS remains Darwin's native TSD base for the entire process, so synchronous
	// faults need no Wibo teardown. Preserve the kernel's default fatal-signal
	// actions: unlike a user-space exit handler, they reliably reap every
	// translated Rosetta thread.
	// A closed diagnostic pipe is not a guest crash. Ignoring SIGPIPE makes the
	// underlying write report EPIPE and prevents a bounded log consumer from
	// terminating a long-running guest process.
	struct sigaction pipeAction{};
	pipeAction.sa_handler = SIG_IGN;
	sigemptyset(&pipeAction.sa_mask);
	(void)sigaction(SIGPIPE, &pipeAction, nullptr);

	// Optional diagnostics retain the kernel's reliable fatal-signal teardown.
	// SA_RESETHAND records the first synchronous fault, then returning retries
	// the faulting instruction under the default action. No allocator, lock, or
	// guest teardown runs from the signal handler.
	const char *crashLogPath = std::getenv("WIBO_CRASH_LOG");
	if (!crashLogPath || crashLogPath[0] == '\0') {
		return;
	}
	// Multiple Wibo-hosted programs may share a diagnostic file. Append one
	// atomic record per process so a later process cannot erase the first fault.
	gDarwinCrashLogFd = open(crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (gDarwinCrashLogFd < 0) {
		return;
	}
	struct sigaction faultAction{};
	faultAction.sa_sigaction = recordDarwinFatalSignal;
	sigemptyset(&faultAction.sa_mask);
	faultAction.sa_flags = SA_SIGINFO | SA_RESETHAND;
	for (const int signalNumber : {SIGBUS, SIGSEGV, SIGILL, SIGFPE}) {
		(void)sigaction(signalNumber, &faultAction, nullptr);
	}
}
#endif

class MainTebScope {
  public:
	explicit MainTebScope(TEB *teb) : mTeb(teb) {}
	MainTebScope(const MainTebScope &) = delete;
	MainTebScope &operator=(const MainTebScope &) = delete;

	~MainTebScope() {
		wibo::uninstallTebForCurrentThread();
		wibo::destroyTib(mTeb);
	}

  private:
	TEB *mTeb;
};

} // namespace

TEB *wibo::allocateTib() {
	auto *newTib = static_cast<TEB *>(wibo::heap::guestMalloc(sizeof(TEB), true));
	if (!newTib) {
		return nullptr;
	}
	tls::initializeTib(newTib);
	newTib->Tib.Self = toGuestPtr(newTib);
	newTib->Peb = toGuestPtr(processPeb);
	return newTib;
}

void wibo::destroyTib(TEB *tibPtr) {
	if (!tibPtr) {
		return;
	}
	tls::cleanupTib(tibPtr);
	wibo::heap::guestFree(tibPtr);
}

void wibo::initializeTibStackInfo(TEB *tibPtr) {
	if (!tibPtr) {
		return;
	}
	void *guestLimit = nullptr;
	void *guestBase = nullptr;
#if defined(__APPLE__) && defined(WIBO_GUEST_64)
	// Same-width guest calls retain the native stack. Register it before the
	// TEB is installed, and retire it on failed installation or teardown.
	bool stackReady = wibo::heap::registerNativeStackForCurrentThread(&guestLimit, &guestBase);
#else
	// Cross-width guest calls need a separate stack below 2GB.
	bool stackReady = wibo::heap::reserveGuestStack(1 * 1024 * 1024, &guestLimit, &guestBase);
#endif
	if (!stackReady) {
		wibo::diagnosticLog("Failed to initialize guest stack\n");
		std::abort();
	}
	tibPtr->Tib.StackLimit = toGuestPtr(guestLimit);
	tibPtr->Tib.StackBase = toGuestPtr(guestBase);
	tibPtr->CurrentStackPointer = guestBase;
	DEBUG_LOG("initializeTibStackInfo: using guest stack base=%p limit=%p\n", tibPtr->Tib.StackBase,
			  tibPtr->Tib.StackLimit);
}

bool wibo::installTibForCurrentThread(TEB *tibPtr) {
	if (!tibPtr) {
		return false;
	}
	if (!tebThreadSetup(tibPtr)) {
#if defined(__APPLE__) && defined(WIBO_GUEST_64)
		wibo::heap::unregisterNativeStackForCurrentThread();
#endif
		return false;
	}
	currentThreadTeb = tibPtr;
	initFpState();
	return true;
}

void wibo::uninstallTebForCurrentThread() {
	TEB *teb = std::exchange(currentThreadTeb, nullptr);
	tebThreadTeardown(teb);
#if defined(__APPLE__) && defined(WIBO_GUEST_64)
	wibo::heap::unregisterNativeStackForCurrentThread();
#endif
}

void wibo::prepareGuestWorkerSignalMask() {
	// Kept as a platform-neutral thread-start hook. Darwin Win64 no longer
	// blocks termination signals now that guest transitions retain native GS.
}

static std::string getExeName(const char *argv0) {
	std::filesystem::path exePath(argv0 ? argv0 : "wibo");
	return exePath.filename().string();
}

static void printHelp(const char *argv0, bool error) {
	const auto exeName = getExeName(argv0);
	FILE *out = error ? stderr : stdout;
	if (error) {
		fprintf(out, "See '%s --help' for usage information.\n", exeName.c_str());
		return;
	}
	fprintf(out, "wibo %s\n\n", wibo::kVersionString);
	fprintf(out, "Usage:\n");
	fprintf(out, "  %s [options] <program.exe> [arguments...]\n", exeName.c_str());
	fprintf(out, "  %s path [subcommand options] <path> [path...]\n", exeName.c_str());
	fprintf(out, "\n");
	fprintf(out, "General Options:\n");
	fprintf(out, "  -h, --help            Show this help message and exit\n");
	fprintf(out, "  -V, --version         Show version information and exit\n");
	fprintf(out, "\n");
	fprintf(out, "Runtime Options:\n");
	fprintf(out, "  -C, --chdir DIR       Change working directory before launching the program\n");
	fprintf(out, "  -D, --debug           Enable debug logging (equivalent to WIBO_DEBUG=1)\n");
	fprintf(out, "      --cmdline STRING  Use STRING as the exact guest command line\n");
	fprintf(out, "                        (includes the program name, e.g. \"test.exe a b c\")\n");
	fprintf(out, "      --                Stop option parsing; following arguments are used\n");
	fprintf(out, "                        verbatim as the guest command line, including the\n");
	fprintf(out, "                        program name\n");
	fprintf(out, "\n");
	fprintf(out, "Subcommands:\n");
	fprintf(out, "  path                  Convert between host and Windows-style paths\n");
	fprintf(out, "                        (see '%s path --help' for details)\n", exeName.c_str());
	fprintf(out, "\n");
	fprintf(out, "Examples:\n");
	fprintf(out, "  # Normal usage\n");
	fprintf(out, "  %s path/to/test.exe a b c\n", exeName.c_str());
	fprintf(out, "  %s -C path/to test.exe a b c\n", exeName.c_str());
	fprintf(out, "\n");
	fprintf(out, "  # Advanced: full control over the guest command line\n");
	fprintf(out, "  %s path/to/test.exe -- test.exe a b c\n", exeName.c_str());
	fprintf(out, "  %s --cmdline 'test.exe a b c' path/to/test.exe\n", exeName.c_str());
	fprintf(out, "  %s -- test.exe a b c\n", exeName.c_str());
}

static void printPathHelp(const char *argv0, bool error) {
	const auto exeName = getExeName(argv0);
	FILE *out = error ? stderr : stdout;
	if (error) {
		fprintf(out, "See '%s path --help' for usage information.\n", exeName.c_str());
		return;
	}
	fprintf(out, "Usage:\n");
	fprintf(out, "  %s path [options] <path> [path...]\n", exeName.c_str());
	fprintf(out, "\n");
	fprintf(out, "Path Options (exactly one required):\n");
	fprintf(out, "  -u, --unix       Convert Windows paths to host (Unix-style) paths\n");
	fprintf(out, "  -w, --windows    Convert host (Unix-style) paths to Windows paths\n");
	fprintf(out, "\n");
	fprintf(out, "General Options:\n");
	fprintf(out, "  -h, --help       Show this help message and exit\n");
	fprintf(out, "\n");
	fprintf(out, "Examples:\n");
	fprintf(out, "  %s path -u 'Z:\\home\\user'\n", exeName.c_str());
	fprintf(out, "  %s path -w /home/user\n", exeName.c_str());
}

static int handlePathCommand(int argc, char **argv, const char *argv0) {
	bool convertToUnix = false;
	bool convertToWindows = false;
	std::vector<const char *> inputs;

	for (int i = 0; i < argc; ++i) {
		const char *arg = argv[i];
		if (strcmp(arg, "-u") == 0 || strcmp(arg, "--unix") == 0) {
			convertToUnix = true;
			continue;
		}
		if (strcmp(arg, "-w") == 0 || strcmp(arg, "--windows") == 0) {
			convertToWindows = true;
			continue;
		}
		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			printPathHelp(argv0, false);
			return 0;
		}
		if (arg[0] == '-' && arg[1] != '\0') {
			fprintf(stderr, "Error: unknown option '%s'.\n", arg);
			printPathHelp(argv0, true);
			return 1;
		}
		inputs.push_back(arg);
	}

	if (convertToUnix == convertToWindows) {
		if (!convertToUnix) {
			printPathHelp(argv0, false);
		} else {
			fprintf(stderr, "Error: cannot specify both --unix and --windows for path conversion.\n");
			printPathHelp(argv0, true);
		}
		return 1;
	}
	if (inputs.empty()) {
		fprintf(stderr, "Error: no paths specified for conversion.\n");
		printPathHelp(argv0, true);
		return 1;
	}

	for (const char *input : inputs) {
		if (convertToUnix) {
			auto hostPath = files::pathFromWindows(input).string();
			fprintf(stdout, "%s\n", hostPath.c_str());
		} else {
			std::filesystem::path hostInput(input);
			std::string windowsPath = files::pathToWindows(hostInput);
			fprintf(stdout, "%s\n", windowsPath.c_str());
		}
	}

	return 0;
}

int main(int argc, char **argv) {
	if (argc >= 2 && strcmp(argv[1], "path") == 0) {
		return handlePathCommand(argc - 2, argv + 2, argv[0]);
	}

	std::string chdirPath;
	bool optionDebug = false;
	bool parsingOptions = true;
	int programIndex = -1;
	std::string cmdLine;
	int bootstrapFd = -1;
	int controlFd = -1;

	for (int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		if (parsingOptions) {
			if (strcmp(arg, "--") == 0) {
				parsingOptions = false;
				continue;
			}
			if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
				printHelp(argv[0], false);
				return 0;
			}
			if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
				fprintf(stdout, "wibo %s\n", wibo::kVersionString);
				return 0;
			}
			if (strncmp(arg, "--cmdline=", 10) == 0) {
				cmdLine = arg + 10;
				continue;
			}
			if (strcmp(arg, "--cmdline") == 0) {
				if (i + 1 >= argc) {
					fprintf(stderr, "Error: '%s' requires a command line argument.\n", arg);
					printHelp(argv[0], true);
					return 1;
				}
				cmdLine = argv[++i];
				continue;
			}
			if (strcmp(arg, "--process-bootstrap") == 0) {
				if (i + 2 >= argc || bootstrapFd >= 0)
					return 1;
				const auto parseDescriptor = [](const char *value, int &descriptor) {
					const char *end = value + std::strlen(value);
					const auto result = std::from_chars(value, end, descriptor);
					return result.ec == std::errc{} && result.ptr == end && descriptor >= 3;
				};
				if (!parseDescriptor(argv[++i], bootstrapFd) || !parseDescriptor(argv[++i], controlFd) ||
					bootstrapFd == controlFd)
					return 1;
				continue;
			}
			if (strcmp(arg, "-D") == 0 || strcmp(arg, "--debug") == 0) {
				optionDebug = true;
				continue;
			}
			if (strncmp(arg, "--chdir=", 8) == 0) {
				chdirPath = arg + 8;
				continue;
			}
			if (strcmp(arg, "-C") == 0 || strcmp(arg, "--chdir") == 0) {
				if (i + 1 >= argc) {
					fprintf(stderr, "Error: '%s' requires a directory argument.\n", arg);
					printHelp(argv[0], true);
					return 1;
				}
				chdirPath = argv[++i];
				continue;
			}
			if (strncmp(arg, "-C", 2) == 0 && arg[2] != '\0') {
				chdirPath = arg + 2;
				continue;
			}
			if (arg[0] == '-' && arg[1] != '\0') {
				fprintf(stderr, "Error: unknown option '%s'.\n", arg);
				printHelp(argv[0], true);
				return 1;
			}
		}

		programIndex = i;
		break;
	}

	if (programIndex == -1 && cmdLine.empty()) {
		if (argc == 1) {
			printHelp(argv[0], false);
			return 0;
		}
		fprintf(stderr, "Error: no program or command line specified.\n");
		printHelp(argv[0], true);
		return 1;
	}

	if (!chdirPath.empty()) {
		if (chdir(chdirPath.c_str()) != 0) {
			std::string message = std::string("Failed to chdir to ") + chdirPath;
			perror(message.c_str());
			return 1;
		}
	}

	if (optionDebug || getenv("WIBO_DEBUG")) {
		wibo::debugEnabled = true;
	}

	if (const char *debugIndentEnv = getenv("WIBO_DEBUG_INDENT")) {
		wibo::debugIndent = std::stoul(debugIndentEnv);
	}

	std::optional<files::StandardHandles> inheritedStandardHandles;
	DWORD bootstrapError = 0;
	if (bootstrapFd >= 0) {
		bootstrapError = wibo::initializeChildProcess(bootstrapFd, controlFd, inheritedStandardHandles);
	}
	wibo::initializeDiagnostics();
	if (bootstrapError) {
		wibo::diagnosticLog("Failed to initialize child process state: %u\n", bootstrapError);
		return 1;
	}
	files::init(inheritedStandardHandles);

	// Create PEB
	PEB *peb = reinterpret_cast<PEB *>(wibo::heap::guestMalloc(sizeof(PEB), true));
	peb->ProcessParameters = toGuestPtr(wibo::heap::guestMalloc(sizeof(RTL_USER_PROCESS_PARAMETERS), true));
#ifdef WIBO_GUEST_64
	peb->ProcessHeap = static_cast<GUEST_PTR>(kernel32::GetProcessHeap());
#endif

	// Create TIB
	TEB *tib = reinterpret_cast<TEB *>(wibo::heap::guestMalloc(sizeof(TEB), true));
	wibo::tls::initializeTib(tib);
	tib->Tib.Self = toGuestPtr(tib);
	tib->Peb = toGuestPtr(peb);
	wibo::processPeb = peb;
	wibo::initializeTibStackInfo(tib);
	if (!wibo::installTibForCurrentThread(tib)) {
		wibo::diagnosticLog("Failed to setup x86 segments and TEB: %s\n", std::strerror(errno));
		wibo::destroyTib(tib);
		return 1;
	}
	MainTebScope mainTebScope(tib);
	kernel32::initializeMainThreadObject();
#if defined(__APPLE__) && defined(WIBO_GUEST_64)
	installDarwinSignalPolicy();
#endif

	// Determine the guest program name
	auto guestArgs = wibo::splitCommandLine(cmdLine.c_str());
	std::string programName;
	if (programIndex != -1) {
		programName = argv[programIndex];
	} else if (!guestArgs.empty()) {
		programName = guestArgs[0];
	}
	if (programName.empty()) {
		wibo::diagnosticLog("No guest program specified\n");
		return 1;
	}

	kernel32::initializeEnvironment();

	// Resolve the guest program path
	const auto pathNamespace =
		programIndex != -1 ? wibo::ExecutablePathNamespace::Host : wibo::ExecutablePathNamespace::Windows;
	std::filesystem::path resolvedGuestPath =
		wibo::resolveExecutable(programName, true, pathNamespace).value_or(std::filesystem::path{});
	if (resolvedGuestPath.empty()) {
		wibo::diagnosticLog("Failed to resolve path to guest program %s\n", programName.c_str());
		return 1;
	}

	// Build guest arguments
	int argIndex = -1;
	bool skipProgramName = false;
	if (programIndex != -1 && argc > programIndex + 1) {
		argIndex = programIndex + 1;
		// With "test.exe -- test 1 2 3", treat everything after -- as the full command line
		if (strcmp(argv[argIndex], "--") == 0) {
			argIndex++;
			skipProgramName = true;
		}
	}
	if (guestArgs.empty() && !skipProgramName) {
		guestArgs.push_back(files::pathToWindows(resolvedGuestPath));
	}
	if (argIndex != -1) {
		for (int i = argIndex; i < argc; ++i) {
			guestArgs.emplace_back(argv[i]);
		}
	}

	// Build a command line
	if (cmdLine.empty()) {
		for (size_t i = 0; i < guestArgs.size(); ++i) {
			if (i != 0) {
				cmdLine += ' ';
			}
			const std::string &arg = guestArgs[i];
			bool needQuotes = arg.find_first_of("\" \t\n") != std::string::npos;
			if (needQuotes)
				cmdLine += '"';
			int backslashes = 0;
			for (const char *p = arg.c_str();; p++) {
				char c = *p;
				if (c == '\\') {
					backslashes++;
					continue;
				}

				// Backslashes are doubled *before quotes*
				for (int j = 0; j < backslashes; j++) {
					cmdLine += '\\';
					if (c == '\0' || c == '"')
						cmdLine += '\\';
				}
				backslashes = 0;

				if (c == '\0')
					break;
				if (c == '\"')
					cmdLine += '\\';
				cmdLine += c;
			}
			if (needQuotes)
				cmdLine += '"';
		}
	}
	if (cmdLine.empty() || cmdLine.back() != '\0') {
		cmdLine.push_back('\0');
	}

	wibo::commandLine = cmdLine;
	wibo::commandLineW = stringToWideString(wibo::commandLine.c_str());
	DEBUG_LOG("Command line: %s\n", wibo::commandLine.c_str());

	wibo::guestExecutablePath = resolvedGuestPath;

	// Build argv/argc
	std::vector<char *> guestArgv;
	guestArgv.reserve(guestArgs.size() + 1);
	for (const auto &arg : guestArgs) {
		guestArgv.push_back(const_cast<char *>(arg.c_str()));
	}
	guestArgv.push_back(nullptr);
	wibo::argv = guestArgv.data();
	wibo::argc = static_cast<int>(guestArgv.size()) - 1;

	wibo::initializeModuleRegistry();

	FILE *f = fopen(resolvedGuestPath.c_str(), "rb");
	if (!f) {
		wibo::diagnosticLog("Failed to open file %s: %s\n", resolvedGuestPath.c_str(), std::strerror(errno));
		return 1;
	}

	auto executable = std::make_unique<wibo::Executable>();
	if (!executable->loadPE(f, true)) {
		fclose(f);
		wibo::diagnosticLog("Failed to load PE image %s\n", resolvedGuestPath.c_str());
		return 1;
	}
	fclose(f);

	const auto entryPoint = reinterpret_cast<EntryProc>(executable->entryPoint);
	if (!entryPoint) {
		wibo::diagnosticLog("Executable %s has no entry point\n", resolvedGuestPath.c_str());
		return 1;
	}

	wibo::mainModule =
		wibo::registerProcessModule(std::move(executable), std::move(resolvedGuestPath), std::move(programName));
	if (!wibo::mainModule || !wibo::mainModule->executable) {
		wibo::diagnosticLog("Failed to register process module\n");
		return 1;
	}
	// Dependency initialization can inspect the process image through the PEB.
	peb->ImageBaseAddress = toGuestPtr(wibo::mainModule->executable->imageBase);
	DEBUG_LOG("Registered main module %s at %p\n", wibo::mainModule->normalizedName.c_str(),
			  wibo::mainModule->executable->imageBase);

	if (!wibo::mainModule->executable->resolveImports()) {
		wibo::diagnosticLog("Failed to resolve imports for main module (DLL initialization failure?)\n");
#if defined(__APPLE__) && defined(WIBO_GUEST_64)
		// abort() enters pthread_kill, where Rosetta can leave the translated
		// process permanently uninterruptible. Import failure is an ordinary
		// loader error: detach the native TEB and terminate without running guest
		// destructors or signal machinery.
		wibo::uninstallTebForCurrentThread();
		_exit(127);
#else
		abort();
#endif
	}
	if (!wibo::initializeModuleTls(*wibo::mainModule)) {
		wibo::diagnosticLog("Failed to initialize TLS for main module\n");
		return 1;
	}

	// Reset last error
	kernel32::setLastError(0);

	// Invoke the damn thing
	call_EntryProc(entryPoint);
	DEBUG_LOG("We came back\n");
	wibo::shutdownModuleRegistry();

	return 1;
}
