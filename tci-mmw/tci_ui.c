/* Adds TCI to the output sound list and an address box on the setup dialog.
   When output is TCI and input is a normal sound card, playback is diverted
   onto the TCI link. When input is TCI and output is a sound card, transmit
   audio from the plugin is played on that card. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_OUT ((HWAVEOUT)(ULONG_PTR)0x711C1001)

extern void __stdcall tci_feed_tx(const SHORT *buf, int count);
extern void __stdcall tci_tx_begin(int rate);
extern void __stdcall tci_tx_end(void);

static MMRESULT (WINAPI *real_waveOutOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT (WINAPI *real_waveOutWrite)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *real_waveOutClose)(HWAVEOUT);
static MMRESULT (WINAPI *real_waveOutPrepareHeader)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *real_waveOutUnprepareHeader)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *real_waveOutReset)(HWAVEOUT);
static MMRESULT (WINAPI *real_waveOutPause)(HWAVEOUT);
static MMRESULT (WINAPI *real_waveOutRestart)(HWAVEOUT);
static MMRESULT (WINAPI *real_waveOutGetDevCaps)(UINT_PTR, LPWAVEOUTCAPS, UINT);

static CRITICAL_SECTION g_ui_cs;
static int g_ui_ready;
static int g_fake_open;
static int g_paused;
static DWORD_PTR g_fake_cb;
static DWORD_PTR g_fake_inst;
static int g_fake_rate = 11025;
static int g_fake_channels = 1;
static int g_fake_block = 2;
static HANDLE g_pace_thread;

typedef struct pace_s { WAVEHDR *hdr; DWORD when; int samples; struct pace_s *next; } pace_t;
static pace_t *g_pace;

static HWAVEOUT g_card;
static HANDLE g_card_done;
static WAVEHDR g_card_hdr;
static short *g_card_buf;
static int g_card_bytes;
static int g_card_stereo;
static int g_card_busy;
static char g_card_name[160];

static void ini_path(char *ini, int n)
{
	char dir[MAX_PATH];
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)ini_path, &self);
	GetModuleFileNameA(self, dir, MAX_PATH);
	{
		char *slash = strrchr(dir, '\\');
		if (slash) *slash = 0;
	}
	_snprintf(ini, (size_t)n, "%s\\Mmsstv.ini", dir);
}

static void read_route(char *in_name, int in_n, char *out_name, int out_n)
{
	char ini[MAX_PATH];
	ini_path(ini, MAX_PATH);
	GetPrivateProfileStringA("TCI", "Input", "", in_name, (DWORD)in_n, ini);
	GetPrivateProfileStringA("TCI", "Output", "", out_name, (DWORD)out_n, ini);
}

int tci_output_is_card(void)
{
	char in_name[160], out_name[160];
	read_route(in_name, 160, out_name, 160);
	if (out_name[0] == 0) return 0;
	return _stricmp(out_name, "TCI") != 0;
}

static int should_divert(void)
{
	char in_name[160], out_name[160];
	read_route(in_name, 160, out_name, 160);
	if (_stricmp(out_name, "TCI") != 0) return 0;
	if (_stricmp(in_name, "TCI") == 0) return 0;
	return 1;
}

static void pace_add(WAVEHDR *hdr, DWORD when, int samples)
{
	pace_t *item = (pace_t *)malloc(sizeof(pace_t));
	if (!item) return;
	item->hdr = hdr;
	item->when = when;
	item->samples = samples;
	EnterCriticalSection(&g_ui_cs);
	item->next = g_pace;
	g_pace = item;
	LeaveCriticalSection(&g_ui_cs);
}

static void complete_hdr(WAVEHDR *hdr)
{
	if (!hdr) return;
	hdr->dwFlags |= WHDR_DONE;
	hdr->dwFlags &= ~WHDR_INQUEUE;
	if (g_fake_cb) {
		void (CALLBACK *cb)(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR) =
			(void (CALLBACK *)(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR))g_fake_cb;
		cb(FAKE_OUT, WOM_DONE, g_fake_inst, (DWORD_PTR)hdr, 0);
	}
}

static void feed_hdr(WAVEHDR *hdr, int samples)
{
	SHORT stack[2048];
	SHORT *mono = stack;
	SHORT *src;
	int i, ch;
	if (!hdr || !hdr->lpData || samples < 1) return;
	ch = g_fake_channels > 0 ? g_fake_channels : 1;
	src = (SHORT *)hdr->lpData;
	if (ch == 1) {
		tci_feed_tx(src, samples);
		return;
	}
	if (samples > 2048) mono = (SHORT *)malloc((size_t)samples * sizeof(SHORT));
	if (!mono) return;
	for (i = 0; i < samples; i++) mono[i] = src[i * ch];
	tci_feed_tx(mono, samples);
	if (mono != stack) free(mono);
}

static DWORD WINAPI pace_main(LPVOID unused)
{
	(void)unused;
	for (;;) {
		pace_t *keep = NULL, *due = NULL, *n;
		DWORD now = GetTickCount();
		EnterCriticalSection(&g_ui_cs);
		while (g_pace) {
			pace_t *it = g_pace;
			g_pace = it->next;
			if ((LONG)(now - it->when) >= 0) { it->next = due; due = it; }
			else { it->next = keep; keep = it; }
		}
		g_pace = keep;
		LeaveCriticalSection(&g_ui_cs);
		while (due) {
			n = due->next;
			feed_hdr(due->hdr, due->samples);
			complete_hdr(due->hdr);
			free(due);
			due = n;
		}
		Sleep(5);
	}
	return 0;
}

static MMRESULT WINAPI hook_waveOutOpen(LPHWAVEOUT phwo, UINT id, LPCWAVEFORMATEX wfx, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
	if (phwo && wfx && should_divert()) {
		if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
		*phwo = FAKE_OUT;
		g_fake_cb = cb;
		g_fake_inst = inst;
		g_fake_rate = wfx->nSamplesPerSec ? (int)wfx->nSamplesPerSec : 11025;
		g_fake_channels = wfx->nChannels ? wfx->nChannels : 1;
		g_fake_block = wfx->nBlockAlign ? wfx->nBlockAlign : (g_fake_channels * 2);
		g_fake_open = 1;
		g_paused = 1;
		tci_tx_begin(g_fake_rate);
		return MMSYSERR_NOERROR;
	}
	return real_waveOutOpen(phwo, id, wfx, cb, inst, flags);
}

static MMRESULT WINAPI hook_waveOutWrite(HWAVEOUT hwo, LPWAVEHDR hdr, UINT len)
{
	int samples, ms;
	if (hwo == FAKE_OUT && hdr && hdr->lpData) {
		samples = g_fake_block > 0 ? (int)(hdr->dwBufferLength / (DWORD)g_fake_block) : (int)(hdr->dwBufferLength / 2);
		if (samples < 1) samples = 1;
		ms = g_fake_rate > 0 ? (samples * 1000) / g_fake_rate : 50;
		if (ms < 10) ms = 10;
		hdr->dwFlags |= WHDR_INQUEUE;
		hdr->dwFlags &= ~WHDR_DONE;
		if (g_paused) pace_add(hdr, 0x7fffffff, samples);
		else pace_add(hdr, GetTickCount() + (DWORD)ms, samples);
		(void)len;
		return MMSYSERR_NOERROR;
	}
	return real_waveOutWrite(hwo, hdr, len);
}

static MMRESULT WINAPI hook_waveOutPrepareHeader(HWAVEOUT hwo, LPWAVEHDR hdr, UINT len)
{
	if (hwo == FAKE_OUT && hdr) {
		hdr->dwFlags |= WHDR_PREPARED;
		(void)len;
		return MMSYSERR_NOERROR;
	}
	return real_waveOutPrepareHeader(hwo, hdr, len);
}

static MMRESULT WINAPI hook_waveOutUnprepareHeader(HWAVEOUT hwo, LPWAVEHDR hdr, UINT len)
{
	if (hwo == FAKE_OUT && hdr) {
		hdr->dwFlags &= ~WHDR_PREPARED;
		(void)len;
		return MMSYSERR_NOERROR;
	}
	return real_waveOutUnprepareHeader(hwo, hdr, len);
}

static void flush_fake(void)
{
	pace_t *all = NULL;
	EnterCriticalSection(&g_ui_cs);
	all = g_pace;
	g_pace = NULL;
	LeaveCriticalSection(&g_ui_cs);
	while (all) {
		pace_t *n = all->next;
		complete_hdr(all->hdr);
		free(all);
		all = n;
	}
}

static MMRESULT WINAPI hook_waveOutReset(HWAVEOUT hwo)
{
	if (hwo == FAKE_OUT) {
		flush_fake();
		tci_tx_end();
		return MMSYSERR_NOERROR;
	}
	return real_waveOutReset(hwo);
}

static MMRESULT WINAPI hook_waveOutClose(HWAVEOUT hwo)
{
	if (hwo == FAKE_OUT) {
		flush_fake();
		tci_tx_end();
		g_fake_open = 0;
		return MMSYSERR_NOERROR;
	}
	return real_waveOutClose(hwo);
}

static MMRESULT WINAPI hook_waveOutPause(HWAVEOUT hwo)
{
	if (hwo == FAKE_OUT) {
		g_paused = 1;
		return MMSYSERR_NOERROR;
	}
	return real_waveOutPause(hwo);
}

static MMRESULT WINAPI hook_waveOutRestart(HWAVEOUT hwo)
{
	pace_t *all, *it;
	DWORD when;
	int step, n;
	if (hwo == FAKE_OUT) {
		EnterCriticalSection(&g_ui_cs);
		all = g_pace;
		g_pace = NULL;
		LeaveCriticalSection(&g_ui_cs);
		when = GetTickCount();
		n = 0;
		for (it = all; it; it = it->next) n++;
		step = 0;
		while (all) {
			pace_t *next = all->next;
			int ms = g_fake_rate > 0 ? (all->samples * 1000) / g_fake_rate : 50;
			if (ms < 10) ms = 10;
			all->when = when + (DWORD)(step * ms);
			step++;
			pace_add(all->hdr, all->when, all->samples);
			free(all);
			all = next;
		}
		g_paused = 0;
		(void)n;
		return MMSYSERR_NOERROR;
	}
	return real_waveOutRestart(hwo);
}

static void patch_import(HMODULE mod, const char *dll, const char *name, void *hook, void **slot)
{
	IMAGE_DOS_HEADER *dos;
	IMAGE_NT_HEADERS *nt;
	IMAGE_IMPORT_DESCRIPTOR *imp;
	if (!mod) return;
	dos = (IMAGE_DOS_HEADER *)mod;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
	nt = (IMAGE_NT_HEADERS *)((BYTE *)mod + dos->e_lfanew);
	if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) return;
	imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)mod + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
	for (; imp->Name; imp++) {
		const char *have = (const char *)mod + imp->Name;
		IMAGE_THUNK_DATA *thunk, *orig;
		if (_stricmp(have, dll) != 0) continue;
		thunk = (IMAGE_THUNK_DATA *)((BYTE *)mod + imp->FirstThunk);
		orig = (IMAGE_THUNK_DATA *)((BYTE *)mod + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
		for (; orig->u1.AddressOfData; orig++, thunk++) {
			IMAGE_IMPORT_BY_NAME *ibn;
			DWORD old;
			if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
			ibn = (IMAGE_IMPORT_BY_NAME *)((BYTE *)mod + orig->u1.AddressOfData);
			if (strcmp((char *)ibn->Name, name) != 0) continue;
			VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
			if (slot && !*slot) *slot = (void *)thunk->u1.Function;
			thunk->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&thunk->u1.Function, sizeof(void *), old, &old);
			return;
		}
	}
}

static int combo_has_tci(HWND cb)
{
	int count = (int)SendMessage(cb, CB_GETCOUNT, 0, 0);
	int i;
	for (i = 0; i < count; i++) {
		if (IsWindowUnicode(cb)) {
			WCHAR w[80];
			SendMessageW(cb, CB_GETLBTEXT, (WPARAM)i, (LPARAM)w);
			if (_wcsicmp(w, L"TCI") == 0) return 1;
		} else {
			char a[80];
			SendMessageA(cb, CB_GETLBTEXT, (WPARAM)i, (LPARAM)a);
			if (_stricmp(a, "TCI") == 0) return 1;
		}
	}
	return 0;
}

static void combo_text(HWND cb, char *out, int n)
{
	if (!cb) { if (n) out[0] = 0; return; }
	if (IsWindowUnicode(cb)) {
		WCHAR w[180];
		GetWindowTextW(cb, w, 180);
		WideCharToMultiByte(CP_ACP, 0, w, -1, out, n, NULL, NULL);
	} else {
		GetWindowTextA(cb, out, n);
	}
}

static void combo_add_tci(HWND cb)
{
	if (!cb || combo_has_tci(cb)) return;
	if (IsWindowUnicode(cb)) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"TCI");
	else SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"TCI");
}

typedef struct {
	HWND dlg;
	HWND in_combo;
	HWND out_combo;
	int best;
} find_t;

static void walk(HWND parent, find_t *f)
{
	HWND c;
	for (c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		char cls[64];
		GetClassNameA(c, cls, 64);
		if (strstr(cls, "Combo") || strstr(cls, "COMBO")) {
			if (combo_has_tci(c)) f->in_combo = c;
		}
		walk(c, f);
	}
}

static void walk_out(HWND parent, find_t *f)
{
	HWND c;
	RECT inr;
	if (!f->in_combo) return;
	GetWindowRect(f->in_combo, &inr);
	for (c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		char cls[64];
		RECT r;
		int dw, dx, dy, score;
		GetClassNameA(c, cls, 64);
		if ((strstr(cls, "Combo") || strstr(cls, "COMBO")) && c != f->in_combo && !combo_has_tci(c)) {
			GetWindowRect(c, &r);
			dw = (r.right - r.left) - (inr.right - inr.left);
			if (dw < 0) dw = -dw;
			dx = r.left - inr.left;
			if (dx < 0) dx = -dx;
			dy = r.top - inr.top;
			if (dy < 0) dy = -dy;
			if (dw <= 8 && dx <= 12 && dy > 0 && dy < 80) {
				score = dy;
				if (!f->out_combo || score < f->best) {
					f->out_combo = c;
					f->best = score;
				}
			}
		}
		walk_out(c, f);
	}
}

static void augment(HWND dlg);
static HWND g_host_edit;
static HWND g_port_edit;
static int g_in_fix;

static void tci_log(const char *msg)
{
	char path[MAX_PATH];
	HANDLE f;
	DWORD n;
	ini_path(path, MAX_PATH);
	{
		char *slash = strrchr(path, '\\');
		if (slash) strcpy(slash + 1, "tci_ui.log");
	}
	f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) return;
	WriteFile(f, msg, (DWORD)strlen(msg), &n, NULL);
	CloseHandle(f);
}

static void save_dialog(HWND in_combo, HWND out_combo)
{
	char ini[MAX_PATH], in_name[160], out_name[160], host[256], port[32];
	combo_text(in_combo, in_name, 160);
	combo_text(out_combo, out_name, 160);
	if (g_host_edit) GetWindowTextA(g_host_edit, host, 256);
	else strcpy(host, "127.0.0.1");
	if (g_port_edit) GetWindowTextA(g_port_edit, port, 32);
	else strcpy(port, "40001");
	if (host[0] == 0) strcpy(host, "127.0.0.1");
	if (port[0] == 0) strcpy(port, "40001");
	ini_path(ini, MAX_PATH);
	WritePrivateProfileStringA("TCI", "Host", host, ini);
	WritePrivateProfileStringA("TCI", "Port", port, ini);
	WritePrivateProfileStringA("TCI", "Input", in_name, ini);
	WritePrivateProfileStringA("TCI", "Output", out_name, ini);
}

static void set_combo(HWND cb, const char *text)
{
	WCHAR w[180];
	if (!cb || !text) return;
	if (IsWindowUnicode(cb)) {
		MultiByteToWideChar(CP_ACP, 0, text, -1, w, 180);
		SetWindowTextW(cb, w);
	} else {
		SetWindowTextA(cb, text);
	}
}

static void remember_boxes(HWND dlg, HWND in_combo, HWND out_combo)
{
	if (in_combo) SetPropA(dlg, "TciIn", in_combo);
	if (out_combo) SetPropA(dlg, "TciOut", out_combo);
}

static void boxes_of(HWND dlg, HWND *in_combo, HWND *out_combo)
{
	find_t f;
	*in_combo = (HWND)GetPropA(dlg, "TciIn");
	*out_combo = (HWND)GetPropA(dlg, "TciOut");
	if (*in_combo && *out_combo) return;
	memset(&f, 0, sizeof(f));
	walk(dlg, &f);
	walk_out(dlg, &f);
	if (!*in_combo) *in_combo = f.in_combo;
	if (!*out_combo) *out_combo = f.out_combo;
	remember_boxes(dlg, *in_combo, *out_combo);
}

static void keep_output_choice(HWND dlg)
{
	HWND in_combo, out_combo;
	char in_name[160], out_name[160], saved_out[160], saved_in[160];
	if (g_in_fix) return;
	g_in_fix = 1;
	boxes_of(dlg, &in_combo, &out_combo);
	if (!out_combo) return;
	combo_add_tci(out_combo);
	EnableWindow(out_combo, TRUE);
	combo_text(in_combo, in_name, 160);
	combo_text(out_combo, out_name, 160);
	read_route(saved_in, 160, saved_out, 160);
	if (out_name[0] == 0 || strcmp(out_name, "-1") == 0) {
		if (saved_out[0] && strcmp(saved_out, "-1") != 0) set_combo(out_combo, saved_out);
		else if (_stricmp(in_name, "TCI") == 0) set_combo(out_combo, "TCI");
	}
	g_in_fix = 0;
}

static LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	WNDPROC old = (WNDPROC)GetPropA(hwnd, "TciOld");
	int unicode = IsWindowUnicode(hwnd);
	if (msg == WM_COMMAND && (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == CBN_SELENDOK)) {
		LRESULT r = unicode ? CallWindowProcW(old, hwnd, msg, wp, lp) : CallWindowProcA(old, hwnd, msg, wp, lp);
		keep_output_choice(hwnd);
		return r;
	}
	if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
		char cap[64];
		HWND in_combo, out_combo;
		GetWindowTextA((HWND)lp, cap, 64);
		if (_stricmp(cap, "OK") == 0) {
			boxes_of(hwnd, &in_combo, &out_combo);
			save_dialog(in_combo, out_combo);
		}
	}
	if (msg == WM_COMMAND && HIWORD(wp) == EN_KILLFOCUS &&
		((HWND)lp == g_host_edit || (HWND)lp == g_port_edit)) {
		HWND in_combo, out_combo;
		boxes_of(hwnd, &in_combo, &out_combo);
		save_dialog(in_combo, out_combo);
	}
	if (msg == WM_APP + 50) {
		tci_log("augment\r\n");
		augment(hwnd);
		tci_log("augment done\r\n");
		return 0;
	}
	if (msg == WM_APP + 51) {
		keep_output_choice(hwnd);
		return 0;
	}
	return unicode ? CallWindowProcW(old, hwnd, msg, wp, lp) : CallWindowProcA(old, hwnd, msg, wp, lp);
}

static void augment(HWND dlg)
{
	find_t f;
	HWND parent;
	RECT r, pr;
	int y, x;
	char ini[MAX_PATH], host[256], port[32];
	if (GetPropA(dlg, "TciBuilt")) return;
	memset(&f, 0, sizeof(f));
	f.dlg = dlg;
	walk(dlg, &f);
	if (!f.in_combo) return;
	walk_out(dlg, &f);
	if (!f.out_combo) return;
	combo_add_tci(f.out_combo);
	EnableWindow(f.out_combo, TRUE);
	parent = GetParent(f.out_combo);
	GetWindowRect(f.out_combo, &r);
	MapWindowPoints(NULL, parent, (POINT *)&r, 2);
	x = r.left;
	GetClientRect(parent, &pr);
	y = pr.bottom + 4;
	{
		RECT wr;
		HWND up = parent;
		GetWindowRect(parent, &wr);
		SetWindowPos(parent, NULL, 0, 0, wr.right - wr.left, (wr.bottom - wr.top) + 30, SWP_NOMOVE | SWP_NOZORDER);
		while (up && (GetWindowLong(up, GWL_STYLE) & WS_CHILD)) {
			HWND p = GetParent(up);
			POINT pt;
			RECT cr, prr;
			if (!p) break;
			GetWindowRect(up, &wr);
			pt.x = wr.left;
			pt.y = wr.bottom;
			MapWindowPoints(NULL, p, &pt, 1);
			GetClientRect(p, &cr);
			if (pt.y + 6 > cr.bottom) {
				GetWindowRect(p, &prr);
				SetWindowPos(p, NULL, 0, 0, prr.right - prr.left, (prr.bottom - prr.top) + (pt.y + 10 - cr.bottom), SWP_NOMOVE | SWP_NOZORDER);
			}
			up = p;
		}
		{
			RECT wr2, cr;
			POINT pt;
			GetWindowRect(parent, &wr2);
			pt.x = wr2.left;
			pt.y = wr2.bottom;
			MapWindowPoints(NULL, dlg, &pt, 1);
			GetClientRect(dlg, &cr);
			if (pt.y + 8 > cr.bottom) {
				GetWindowRect(dlg, &wr2);
				SetWindowPos(dlg, NULL, 0, 0, wr2.right - wr2.left, (wr2.bottom - wr2.top) + (pt.y + 12 - cr.bottom), SWP_NOMOVE | SWP_NOZORDER);
			}
		}
	}
	ini_path(ini, MAX_PATH);
	GetPrivateProfileStringA("TCI", "Host", "127.0.0.1", host, sizeof(host), ini);
	GetPrivateProfileStringA("TCI", "Port", "40001", port, sizeof(port), ini);
	{
		HWND label, host_edit, port_edit;
		HFONT font = (HFONT)SendMessage(parent, WM_GETFONT, 0, 0);
		label = CreateWindowExW(0, L"STATIC", L"Address", WS_CHILD | WS_VISIBLE, 4, y + 3, 58, 16, parent, NULL, NULL, NULL);
		host_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, 96, 22, parent, NULL, NULL, NULL);
		port_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x + 100, y, 48, 22, parent, NULL, NULL, NULL);
		if (font) {
			SendMessage(label, WM_SETFONT, (WPARAM)font, TRUE);
			SendMessage(host_edit, WM_SETFONT, (WPARAM)font, TRUE);
			SendMessage(port_edit, WM_SETFONT, (WPARAM)font, TRUE);
		}
		g_host_edit = host_edit;
		g_port_edit = port_edit;
		SetWindowTextA(g_host_edit, host);
		SetWindowTextA(g_port_edit, port);
	}
	remember_boxes(dlg, f.in_combo, f.out_combo);
	SetPropA(dlg, "TciBuilt", (HANDLE)1);
	keep_output_choice(dlg);
}

static HHOOK g_call_hook;
static HWND g_call_target;

static LRESULT CALLBACK call_hook_proc(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION) {
		CWPSTRUCT *cwp = (CWPSTRUCT *)lp;
		if (cwp->hwnd == g_call_target && !GetPropA(cwp->hwnd, "TciHooked")) {
			HWND dlg = g_call_target;
			HHOOK hk = g_call_hook;
			WNDPROC old = (WNDPROC)(IsWindowUnicode(dlg) ? GetWindowLongPtrW(dlg, GWLP_WNDPROC) : GetWindowLongPtrA(dlg, GWLP_WNDPROC));
			tci_log("subclass\r\n");
			SetPropA(dlg, "TciOld", (HANDLE)old);
			if (IsWindowUnicode(dlg)) SetWindowLongPtrW(dlg, GWLP_WNDPROC, (LONG_PTR)dlg_proc);
			else SetWindowLongPtrA(dlg, GWLP_WNDPROC, (LONG_PTR)dlg_proc);
			SetPropA(dlg, "TciHooked", (HANDLE)1);
			g_call_hook = NULL;
			PostMessage(dlg, WM_APP + 50, 0, 0);
			if (hk) UnhookWindowsHookEx(hk);
		}
	}
	return CallNextHookEx(NULL, code, wp, lp);
}

static void look_for_dialog(void)
{
	HWND w = NULL;
	DWORD pid = GetCurrentProcessId();
	for (w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT)) {
		DWORD wpid = 0;
		find_t f;
		GetWindowThreadProcessId(w, &wpid);
		if (wpid != pid || !IsWindowVisible(w)) continue;
		memset(&f, 0, sizeof(f));
		walk(w, &f);
		if (f.in_combo) {
			if (GetPropA(w, "TciHooked")) SendMessage(w, WM_APP + 51, 0, 0);
			else if (!g_call_hook) {
				DWORD tid = GetWindowThreadProcessId(w, NULL);
				HMODULE self = NULL;
				GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCSTR)look_for_dialog, &self);
				g_call_target = w;
				g_call_hook = SetWindowsHookExA(WH_CALLWNDPROC, call_hook_proc, self, tid);
				if (g_call_hook) SendMessage(w, WM_NULL, 0, 0);
				else augment(w);
			}
		}
	}
}

static UINT find_card(const char *name)
{
	UINT n, i;
	if (!real_waveOutGetDevCaps) return WAVE_MAPPER;
	n = waveOutGetNumDevs();
	for (i = 0; i < n; i++) {
		WAVEOUTCAPS caps;
		if (real_waveOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
			if (_stricmp(caps.szPname, name) == 0) return i;
		}
	}
	return WAVE_MAPPER;
}

LONG tci_card_open(DWORD sampfreq, DWORD size)
{
	char in_name[160], out_name[160];
	WAVEFORMATEX wfx;
	MMRESULT r;
	UINT id;
	read_route(in_name, 160, out_name, 160);
	if (g_card) return 1;
	strncpy(g_card_name, out_name, sizeof(g_card_name) - 1);
	id = find_card(g_card_name);
	memset(&wfx, 0, sizeof(wfx));
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = sampfreq ? sampfreq : 11025;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = 2;
	wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
	if (!g_card_done) g_card_done = CreateEventA(NULL, FALSE, FALSE, NULL);
	g_card_stereo = 0;
	r = real_waveOutOpen(&g_card, id, &wfx, (DWORD_PTR)0, 0, CALLBACK_NULL);
	if (r != MMSYSERR_NOERROR) {
		wfx.nChannels = 2;
		wfx.nBlockAlign = 4;
		wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
		g_card_stereo = 1;
		r = real_waveOutOpen(&g_card, id, &wfx, 0, 0, CALLBACK_NULL);
		if (r != MMSYSERR_NOERROR) { g_card = NULL; return 0; }
	}
	g_card_bytes = (int)(size ? size : 2048) * wfx.nBlockAlign;
	free(g_card_buf);
	g_card_buf = (short *)malloc((size_t)g_card_bytes);
	g_card_busy = 0;
	return 1;
}

void tci_card_abort(void)
{
	if (g_card_busy && g_card) {
		real_waveOutReset(g_card);
		g_card_busy = 0;
	}
	if (g_card) {
		if (g_card_hdr.dwFlags & WHDR_PREPARED)
			real_waveOutUnprepareHeader(g_card, &g_card_hdr, sizeof(g_card_hdr));
		real_waveOutClose(g_card);
		g_card = NULL;
	}
	free(g_card_buf);
	g_card_buf = NULL;
}

BOOL tci_card_write(const SHORT *buf, int count)
{
	int i;
	if (!g_card || !g_card_buf || count < 1) return FALSE;
	if (g_card_busy) {
		DWORD start = GetTickCount();
		while (GetTickCount() - start < 2000) {
			if (!(g_card_hdr.dwFlags & WHDR_INQUEUE) || (g_card_hdr.dwFlags & WHDR_DONE)) break;
			Sleep(5);
		}
		real_waveOutUnprepareHeader(g_card, &g_card_hdr, sizeof(g_card_hdr));
		g_card_busy = 0;
	}
	if (g_card_stereo) {
		for (i = 0; i < count; i++) {
			g_card_buf[i * 2] = buf[i];
			g_card_buf[i * 2 + 1] = buf[i];
		}
	} else {
		memcpy(g_card_buf, buf, (size_t)count * sizeof(short));
	}
	memset(&g_card_hdr, 0, sizeof(g_card_hdr));
	g_card_hdr.lpData = (LPSTR)g_card_buf;
	g_card_hdr.dwBufferLength = (DWORD)(count * (g_card_stereo ? 4 : 2));
	if (real_waveOutPrepareHeader(g_card, &g_card_hdr, sizeof(g_card_hdr)) != MMSYSERR_NOERROR) return FALSE;
	if (real_waveOutWrite(g_card, &g_card_hdr, sizeof(g_card_hdr)) != MMSYSERR_NOERROR) return FALSE;
	g_card_busy = 1;
	return TRUE;
}

static volatile LONG g_rig_hz;
static LONG g_shown_hz;
static HWND g_freq_win;
static HWND g_band_combo;
static HWND g_log_box;
static HBRUSH g_freq_bg;
static char g_freq_text[64] = "FREQUENCY";
static HFONT g_freq_font;

void tci_note_freq(unsigned long hz)
{
	if (hz > 1000) InterlockedExchange(&g_rig_hz, (LONG)hz);
}

static int band_for(unsigned long hz, char *band, int nb, char *disp, int nd)
{
	struct { unsigned long lo, hi; int digits; const char *name; } rows[] = {
		{1810000, 2700000, 1, "160M"},
		{3500000, 3800000, 1, "80M"},
		{5351500, 5366500, 1, "60M"},
		{7000000, 7200000, 1, "40M"},
		{10000000, 10150000, 2, "30M"},
		{14000000, 14350000, 2, "20M"},
		{18068000, 18168000, 2, "17M"},
		{21000000, 21450000, 2, "15M"},
		{24890000, 24990000, 2, "12M"},
		{26965000, 27405000, 2, "11M"},
		{27700000, 27700000, 2, "11M"},
		{28000000, 29700000, 2, "10M"},
		{50000000, 52000000, 2, "6M"},
		{70150000, 70200000, 2, "4M"},
		{144000000, 146000000, 3, "2M"},
		{430000000, 440000000, 3, "70CM"}
	};
	int i, mhz, khz, digits;
	char num[16], ms[8], ks[8];
	for (i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++) {
		if (hz < rows[i].lo || hz > rows[i].hi) continue;
		digits = rows[i].digits;
		_snprintf(num, sizeof(num), "%lu", hz);
		if ((int)strlen(num) < digits + 3) {
			mhz = (int)(hz / 1000000UL);
			khz = (int)((hz / 1000UL) % 1000UL);
		} else {
			memcpy(ms, num, (size_t)digits);
			ms[digits] = 0;
			memcpy(ks, num + digits, 3);
			ks[3] = 0;
			mhz = atoi(ms);
			khz = atoi(ks);
		}
		_snprintf(band, (size_t)nb, "%s", rows[i].name);
		_snprintf(disp, (size_t)nd, " %d.%03dKhz->%s", mhz, khz, rows[i].name);
		return 1;
	}
	_snprintf(band, (size_t)nb, "%s", "");
	_snprintf(disp, (size_t)nd, "%s", " FUERA DE BANDA");
	return 0;
}

static LRESULT CALLBACK freq_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_ERASEBKGND) {
		RECT r;
		HDC dc = (HDC)wp;
		GetClientRect(hwnd, &r);
		if (!g_freq_bg) g_freq_bg = CreateSolidBrush(RGB(32, 32, 32));
		FillRect(dc, &r, g_freq_bg);
		return 1;
	}
	if (msg == WM_PAINT) {
		PAINTSTRUCT ps;
		RECT r;
		HDC dc = BeginPaint(hwnd, &ps);
		HFONT old;
		GetClientRect(hwnd, &r);
		if (!g_freq_bg) g_freq_bg = CreateSolidBrush(RGB(32, 32, 32));
		FillRect(dc, &r, g_freq_bg);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(255, 255, 255));
		old = (HFONT)SelectObject(dc, g_freq_font ? g_freq_font : GetStockObject(ANSI_FIXED_FONT));
		DrawTextA(dc, g_freq_text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, old);
		EndPaint(hwnd, &ps);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

static int combo_has_text_item(HWND cb, const wchar_t *want)
{
	int count = (int)SendMessage(cb, CB_GETCOUNT, 0, 0);
	int i;
	for (i = 0; i < count; i++) {
		if (IsWindowUnicode(cb)) {
			WCHAR w[40];
			SendMessageW(cb, CB_GETLBTEXT, (WPARAM)i, (LPARAM)w);
			if (_wcsicmp(w, want) == 0) return 1;
		} else {
			char a[40];
			SendMessageA(cb, CB_GETLBTEXT, (WPARAM)i, (LPARAM)a);
			if (_stricmp(a, "160M") == 0 && wcscmp(want, L"160M") == 0) return 1;
		}
	}
	return 0;
}

static void find_band_combo(HWND parent)
{
	HWND c;
	for (c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		char cls[64];
		GetClassNameA(c, cls, 64);
		if ((strstr(cls, "Combo") || strstr(cls, "COMBO")) && combo_has_text_item(c, L"160M") && combo_has_text_item(c, L"20M"))
			g_band_combo = c;
		find_band_combo(c);
	}
}

static void set_band_text(HWND cb, const char *text)
{
	char cur[40];
	WCHAR w[40];
	int idx;
	if (!cb || !text || !text[0]) return;
	combo_text(cb, cur, 40);
	if (_stricmp(cur, text) == 0) return;
	if (IsWindowUnicode(cb)) {
		MultiByteToWideChar(CP_ACP, 0, text, -1, w, 40);
		idx = (int)SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)w);
		if (idx >= 0) SendMessageW(cb, CB_SETCURSEL, (WPARAM)idx, 0);
		else SetWindowTextW(cb, w);
	} else {
		idx = (int)SendMessageA(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)text);
		if (idx >= 0) SendMessageA(cb, CB_SETCURSEL, (WPARAM)idx, 0);
		else SetWindowTextA(cb, text);
	}
}

static void find_log(HWND parent)
{
	HWND c;
	for (c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		char cls[64], txt[32];
		GetClassNameA(c, cls, 64);
		GetWindowTextA(c, txt, 32);
		if (strstr(cls, "Group") && _stricmp(txt, "LOG") == 0) g_log_box = c;
		find_log(c);
	}
}

static void place_freq(HWND main)
{
	RECT wr;
	int gw, gh, x, y, w, h;
	POINT pt;
	if (g_rig_hz < 1000) {
		if (g_freq_win) ShowWindow(g_freq_win, SW_HIDE);
		return;
	}
	if (!g_log_box || !IsWindow(g_log_box)) {
		g_log_box = NULL;
		find_log(main);
	}
	if (!g_log_box) return;
	GetWindowRect(g_log_box, &wr);
	gw = wr.right - wr.left;
	gh = wr.bottom - wr.top;
	if (gw < 100 || gh < 40) return;
	/* Label1 is Left 255, Top 131, Width 108, Height 25 inside the design 367x164 LOG group.
	   On the live window the glyphs sit just under the RxID/TxID buttons. */
	pt.x = wr.left + (gw * 255) / 367;
	pt.y = wr.top + (gh * 128) / 164;
	w = (gw * 108) / 367;
	h = (gh * 32) / 164;
	if (w < 70) w = 70;
	if (h < 16) h = 16;
	MapWindowPoints(NULL, main, &pt, 1);
	x = pt.x;
	y = pt.y;
	if (!g_freq_win) {
		WNDCLASSW wc;
		memset(&wc, 0, sizeof(wc));
		wc.lpfnWndProc = freq_proc;
		wc.hInstance = GetModuleHandle(NULL);
		wc.lpszClassName = L"YoniqTciFreq";
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		RegisterClassW(&wc);
		g_freq_win = CreateWindowExW(0, L"YoniqTciFreq", L"", WS_CHILD | WS_VISIBLE, x, y, w, h, main, NULL, GetModuleHandle(NULL), NULL);
	}
	{
		static int font_px;
		int fh = h - 12;
		if (fh > w / 10) fh = w / 10;
		if (fh < 12) fh = 12;
		if (fh != font_px) {
			HFONT nf = CreateFontA(-fh, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
				FIXED_PITCH | FF_MODERN, "Courier New");
			if (nf) {
				if (g_freq_font) DeleteObject(g_freq_font);
				g_freq_font = nf;
				font_px = fh;
			}
		}
	}
	if (g_freq_win) {
		SetWindowPos(g_freq_win, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
		InvalidateRect(g_freq_win, NULL, TRUE);
	}
}

static void apply_rig(HWND main)
{
	LONG hz = g_rig_hz;
	char band[16], disp[64];
	if (!main) return;
	if (!g_band_combo || !IsWindow(g_band_combo)) {
		g_band_combo = NULL;
		find_band_combo(main);
	}
	if (hz > 1000 && hz != g_shown_hz) {
		band_for((unsigned long)hz, band, 16, disp, 64);
		_snprintf(g_freq_text, sizeof(g_freq_text), "%s", disp);
		set_band_text(g_band_combo, band);
		g_shown_hz = hz;
	}
	place_freq(main);
}

static HWND g_main_target;
static HHOOK g_main_hook;
static LRESULT CALLBACK main_rig_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static LRESULT CALLBACK main_hook_proc(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION) {
		CWPSTRUCT *cwp = (CWPSTRUCT *)lp;
		if (cwp->hwnd == g_main_target && !GetPropA(cwp->hwnd, "TciMain")) {
			HWND main = g_main_target;
			HHOOK hk = g_main_hook;
			WNDPROC old = (WNDPROC)(IsWindowUnicode(main) ? GetWindowLongPtrW(main, GWLP_WNDPROC) : GetWindowLongPtrA(main, GWLP_WNDPROC));
			SetPropA(main, "TciMainOld", (HANDLE)old);
			if (IsWindowUnicode(main)) SetWindowLongPtrW(main, GWLP_WNDPROC, (LONG_PTR)main_rig_proc);
			else SetWindowLongPtrA(main, GWLP_WNDPROC, (LONG_PTR)main_rig_proc);
			SetPropA(main, "TciMain", (HANDLE)1);
			g_main_hook = NULL;
			PostMessage(main, WM_APP + 61, 0, 0);
			if (hk) UnhookWindowsHookEx(hk);
		}
	}
	return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK main_rig_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	WNDPROC old = (WNDPROC)GetPropA(hwnd, "TciMainOld");
	int unicode = IsWindowUnicode(hwnd);
	if (msg == WM_APP + 61) {
		apply_rig(hwnd);
		return 0;
	}
	return unicode ? CallWindowProcW(old, hwnd, msg, wp, lp) : CallWindowProcA(old, hwnd, msg, wp, lp);
}

static void sync_rig_window(void)
{
	HWND w;
	DWORD pid = GetCurrentProcessId();
	for (w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT)) {
		DWORD wpid = 0;
		char cls[64];
		GetWindowThreadProcessId(w, &wpid);
		if (wpid != pid || !IsWindowVisible(w)) continue;
		GetClassNameA(w, cls, 64);
		if (strcmp(cls, "TMmsstv") != 0) continue;
		if (GetPropA(w, "TciMain")) {
			PostMessage(w, WM_APP + 61, 0, 0);
		} else if (!g_main_hook) {
			DWORD tid = GetWindowThreadProcessId(w, NULL);
			HMODULE self = NULL;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)sync_rig_window, &self);
			g_main_target = w;
			g_main_hook = SetWindowsHookExA(WH_CALLWNDPROC, main_hook_proc, self, tid);
			if (g_main_hook) SendMessage(w, WM_NULL, 0, 0);
		}
	}
}

DWORD WINAPI tci_install_hooks(LPVOID arg)
{
	HMODULE winmm;
	(void)arg;
	Sleep(400);
	if (g_ui_ready) return 0;
	g_ui_ready = 1;
	InitializeCriticalSection(&g_ui_cs);
	winmm = LoadLibraryA("winmm.dll");
	real_waveOutOpen = (void *)GetProcAddress(winmm, "waveOutOpen");
	real_waveOutWrite = (void *)GetProcAddress(winmm, "waveOutWrite");
	real_waveOutClose = (void *)GetProcAddress(winmm, "waveOutClose");
	real_waveOutPrepareHeader = (void *)GetProcAddress(winmm, "waveOutPrepareHeader");
	real_waveOutUnprepareHeader = (void *)GetProcAddress(winmm, "waveOutUnprepareHeader");
	real_waveOutReset = (void *)GetProcAddress(winmm, "waveOutReset");
	real_waveOutPause = (void *)GetProcAddress(winmm, "waveOutPause");
	real_waveOutRestart = (void *)GetProcAddress(winmm, "waveOutRestart");
	real_waveOutGetDevCaps = (void *)GetProcAddress(winmm, "waveOutGetDevCapsA");
	patch_import(GetModuleHandle(NULL), "WINMM.DLL", "waveOutOpen", hook_waveOutOpen, (void **)&real_waveOutOpen);
	patch_import(GetModuleHandle(NULL), "WINMM.DLL", "waveOutWrite", hook_waveOutWrite, (void **)&real_waveOutWrite);
	patch_import(GetModuleHandle(NULL), "WINMM.DLL", "waveOutClose", hook_waveOutClose, (void **)&real_waveOutClose);
	patch_import(GetModuleHandle(NULL), "WINMM.DLL", "waveOutPrepareHeader", hook_waveOutPrepareHeader, (void **)&real_waveOutPrepareHeader);
	patch_import(GetModuleHandle(NULL), "WINMM.DLL", "waveOutUnprepareHeader", hook_waveOutUnprepareHeader, (void **)&real_waveOutUnprepareHeader);
	patch_import(GetModuleHandle(NULL), "WINMM.DLL", "waveOutReset", hook_waveOutReset, (void **)&real_waveOutReset);
	patch_import(GetModuleHandle(NULL), "WINMM.DLL", "waveOutPause", hook_waveOutPause, (void **)&real_waveOutPause);
	patch_import(GetModuleHandle(NULL), "WINMM.DLL", "waveOutRestart", hook_waveOutRestart, (void **)&real_waveOutRestart);
	g_pace_thread = CreateThread(NULL, 0, pace_main, NULL, 0, NULL);
	for (;;) {
		look_for_dialog();
		sync_rig_window();
		Sleep(400);
	}
	return 0;
}
