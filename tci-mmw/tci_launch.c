/* Starts YONIQ and loads TCI.mmw so the Misc tab can show TCI on output and the address box. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static void note(const char *path, const char *msg)
{
	HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD n;
	if (f == INVALID_HANDLE_VALUE) return;
	WriteFile(f, msg, (DWORD)strlen(msg), &n, NULL);
	CloseHandle(f);
}

static int inject(HANDLE process, const char *dll, const char *log)
{
	SIZE_T n = strlen(dll) + 1;
	LPVOID remote;
	HANDLE thread;
	FARPROC load;
	DWORD code = 0;
	char line[256];
	remote = VirtualAllocEx(process, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remote) {
		_snprintf(line, sizeof(line), "alloc failed %lu\r\n", GetLastError());
		note(log, line);
		return 0;
	}
	if (!WriteProcessMemory(process, remote, dll, n, NULL)) {
		_snprintf(line, sizeof(line), "write failed %lu\r\n", GetLastError());
		note(log, line);
		return 0;
	}
	load = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
	thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load, remote, 0, NULL);
	if (!thread) {
		_snprintf(line, sizeof(line), "thread failed %lu\r\n", GetLastError());
		note(log, line);
		return 0;
	}
	if (WaitForSingleObject(thread, 8000) != WAIT_OBJECT_0) note(log, "thread wait timed out\r\n");
	GetExitCodeThread(thread, &code);
	_snprintf(line, sizeof(line), "load result %lu\r\n", code);
	note(log, line);
	CloseHandle(thread);
	return code != 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	char exe[MAX_PATH];
	char dll[MAX_PATH];
	char dir[MAX_PATH];
	char log[MAX_PATH];
	(void)inst; (void)prev; (void)cmd; (void)show;
	GetModuleFileNameA(NULL, dir, MAX_PATH);
	{
		char *slash = strrchr(dir, '\\');
		if (slash) *slash = 0;
	}
	_snprintf(exe, sizeof(exe), "%s\\Yoniq_core.exe", dir);
	_snprintf(dll, sizeof(dll), "%s\\TCI.mmw", dir);
	_snprintf(log, sizeof(log), "%s\\tci_inject.log", dir);
	note(log, "start\r\n");
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
		MessageBoxA(NULL, "Could not start Yoniq_core.exe", "YONIQ", MB_ICONERROR);
		return 1;
	}
	inject(pi.hProcess, dll, log);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}
