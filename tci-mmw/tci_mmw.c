/* TCI.mmw - sound plugin for the installed YONIQ / MMSSTV.
   The Misc tab lists every *.mmw file as an input sound device.
   Selecting TCI carries both receive and transmit audio over the
   Expert Electronics TCI WebSocket. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tci_output_is_card(void);
LONG tci_card_open(DWORD sampfreq, DWORD size);
void tci_card_abort(void);
BOOL tci_card_write(const SHORT *buf, int count);
DWORD WINAPI tci_install_hooks(LPVOID arg);

#define HDR 64
#define RX_AUDIO 1
#define TX_AUDIO 2
#define TX_CHRONO 3

static CRITICAL_SECTION g_send_cs;
static CRITICAL_SECTION g_audio_cs;
static HANDLE g_thread;
static HANDLE g_rx_event;
static SOCKET g_sock = INVALID_SOCKET;
static volatile LONG g_stop;
static volatile LONG g_connected;
static volatile LONG g_ready;
static volatile LONG g_failed;
static volatile LONG g_audio_on;
static volatile LONG g_want_in;
static volatile LONG g_want_out;
static volatile LONG g_ptt;
static int g_wsa;
static char g_host[256] = "127.0.0.1";
static int g_port = 40001;
static int g_rx_num;
static char g_in_sel[160];
static char g_out_sel[160];
static int g_app_rate = 11025;
static int g_tci_rate = 12000;
static int g_block = 2048;
static int g_bcc = 0x7fffffff;
static int g_bcc_frac;

#define RX_CAP (48000 * 3)
#define TX_CAP (48000 * 4)
static short g_rx[RX_CAP];
static int g_rx_head;
static int g_rx_count;
static float g_tx[TX_CAP];
static int g_tx_head;
static int g_tx_count;

static float g_rx_src[16384];
static int g_rx_src_n;
static double g_rx_pos;
static double g_rx_step;
static float g_tx_src[16384];
static int g_tx_src_n;
static double g_tx_pos;
static double g_tx_step;

static char g_text[65536];
static int g_text_n;
static unsigned char *g_frag;
static int g_frag_n;
static int g_frag_cap;
static int g_frag_op = -1;

static int pick_rate(int app)
{
	static const int rates[4] = {8000, 12000, 24000, 48000};
	int best = 12000, best_d = 100000000, i;
	if (app < 1000) app = 11025;
	for (i = 0; i < 4; i++) {
		int d = app - rates[i];
		if (d < 0) d = -d;
		if (d < best_d) { best_d = d; best = rates[i]; }
	}
	return best;
}

static int samples_per_packet(int rate)
{
	if (rate <= 8000) return 128;
	if (rate <= 12000) return 256;
	if (rate <= 24000) return 512;
	return 1024;
}

static void load_config(void)
{
	char dir[MAX_PATH], ini[MAX_PATH];
	HMODULE self = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)load_config, &self);
	if (!GetModuleFileNameA(self, dir, MAX_PATH)) return;
	{
		char *slash = strrchr(dir, '\\');
		if (slash) *slash = 0;
	}
	_snprintf(ini, sizeof(ini), "%s\\Mmsstv.ini", dir);
	GetPrivateProfileStringA("TCI", "Host", "127.0.0.1", g_host, sizeof(g_host), ini);
	g_port = GetPrivateProfileIntA("TCI", "Port", 40001, ini);
	g_rx_num = GetPrivateProfileIntA("TCI", "Receiver", 0, ini);
	if (g_host[0] == 0) strcpy(g_host, "127.0.0.1");
	if (g_port < 1 || g_port > 65535) g_port = 40001;
	if (g_rx_num < 0 || g_rx_num > 7) g_rx_num = 0;
	GetPrivateProfileStringA("TCI", "Input", "", g_in_sel, sizeof(g_in_sel), ini);
	GetPrivateProfileStringA("TCI", "Output", "", g_out_sel, sizeof(g_out_sel), ini);
	WritePrivateProfileStringA("TCI", "Host", g_host, ini);
	{
		char num[16];
		_snprintf(num, sizeof(num), "%d", g_port);
		WritePrivateProfileStringA("TCI", "Port", num, ini);
		_snprintf(num, sizeof(num), "%d", g_rx_num);
		WritePrivateProfileStringA("TCI", "Receiver", num, ini);
	}
}

static void resamp_reset(float *src, int *n, double *pos, double *step, double src_rate, double dst_rate)
{
	*n = 0;
	*pos = 0;
	if (dst_rate < 1) dst_rate = 1;
	if (src_rate < 1) src_rate = dst_rate;
	*step = src_rate / dst_rate;
	if (*step < 0.0001) *step = 0.0001;
	(void)src;
}

static void resamp_push(float *src, int *n, int cap, float s)
{
	if (*n < cap) src[(*n)++] = s;
}

static int resamp_pull(float *src, int *n, double *pos, double step, float *out, int maxn)
{
	int produced = 0;
	while (produced < maxn && (*pos + 1.0) < (double)*n) {
		int i = (int)*pos;
		double frac = *pos - (double)i;
		out[produced++] = (float)(src[i] * (1.0 - frac) + src[i + 1] * frac);
		*pos += step;
	}
	{
		int drop = (int)*pos;
		if (drop > 0 && *n > 0) {
			if (drop >= *n) {
				float last = src[*n - 1];
				src[0] = last;
				*n = 1;
				*pos = 0;
			} else {
				memmove(src, src + drop, (size_t)(*n - drop) * sizeof(float));
				*n -= drop;
				*pos -= drop;
			}
		}
	}
	return produced;
}

static int send_all(const unsigned char *data, int len)
{
	int sent = 0;
	while (sent < len) {
		int n = send(g_sock, (const char *)data + sent, len - sent, 0);
		if (n <= 0) return 0;
		sent += n;
	}
	return 1;
}

static int send_frame(int opcode, const unsigned char *data, int len)
{
	unsigned char mask[4], hdr[8];
	unsigned char *packet;
	int hlen = 2, i, ok;
	if (g_sock == INVALID_SOCKET) return 0;
	for (i = 0; i < 4; i++) mask[i] = (unsigned char)(rand() & 255);
	hdr[0] = (unsigned char)(0x80 | opcode);
	if (len < 126) hdr[1] = (unsigned char)(0x80 | len);
	else {
		hdr[1] = (unsigned char)(0x80 | 126);
		hdr[2] = (unsigned char)((len >> 8) & 255);
		hdr[3] = (unsigned char)(len & 255);
		hlen = 4;
	}
	memcpy(hdr + hlen, mask, 4);
	hlen += 4;
	packet = (unsigned char *)malloc((size_t)hlen + (size_t)len);
	if (!packet) return 0;
	memcpy(packet, hdr, (size_t)hlen);
	for (i = 0; i < len; i++) {
		unsigned char b = data ? data[i] : 0;
		packet[hlen + i] = (unsigned char)(b ^ mask[i & 3]);
	}
	ok = send_all(packet, hlen + len);
	free(packet);
	return ok;
}

static int send_text(const char *command)
{
	int ok;
	EnterCriticalSection(&g_send_cs);
	ok = send_frame(1, (const unsigned char *)command, (int)strlen(command));
	LeaveCriticalSection(&g_send_cs);
	return ok;
}

static void wr_u32(unsigned char *p, unsigned int v) { memcpy(p, &v, 4); }
static unsigned int rd_u32(const unsigned char *p) { unsigned int v; memcpy(&v, p, 4); return v; }

static float read_sample(const unsigned char *data, int *offset, int format)
{
	if (format == 0) {
		short v;
		memcpy(&v, data + *offset, 2);
		*offset += 2;
		return v / 32768.f;
	}
	if (format == 1) {
		int v = data[*offset] | (data[*offset + 1] << 8) | (data[*offset + 2] << 16);
		if (v & 0x800000) v |= ~0xFFFFFF;
		*offset += 3;
		return v / 8388608.f;
	}
	if (format == 2) {
		int v;
		memcpy(&v, data + *offset, 4);
		*offset += 4;
		return v / 2147483648.f;
	}
	{
		float v;
		memcpy(&v, data + *offset, 4);
		*offset += 4;
		return v;
	}
}

static void note_sent(int samples)
{
	if (g_bcc > 0x100000 || g_block < 1 || samples < 1) return;
	g_bcc_frac += samples;
	while (g_bcc_frac >= g_block && g_bcc > -1) {
		g_bcc_frac -= g_block;
		g_bcc--;
	}
}

static void send_tx_audio(int count)
{
	int i, real = 0;
	unsigned char *packet;
	float *samples;
	if (count < 1) count = 1;
	if (count > 8192) count = 8192;
	samples = (float *)calloc((size_t)count, sizeof(float));
	packet = (unsigned char *)calloc((size_t)HDR + (size_t)count * 4, 1);
	if (!samples || !packet) { free(samples); free(packet); return; }
	EnterCriticalSection(&g_audio_cs);
	if (g_ptt) {
		for (i = 0; i < count && g_tx_count > 0; i++) {
			samples[i] = g_tx[g_tx_head];
			g_tx_head = (g_tx_head + 1) % TX_CAP;
			g_tx_count--;
			real++;
		}
		note_sent(real);
	}
	LeaveCriticalSection(&g_audio_cs);
	wr_u32(packet + 0, (unsigned int)g_rx_num);
	wr_u32(packet + 4, (unsigned int)g_tci_rate);
	wr_u32(packet + 8, 3);
	wr_u32(packet + 20, (unsigned int)count);
	wr_u32(packet + 24, TX_AUDIO);
	wr_u32(packet + 28, 1);
	memcpy(packet + HDR, samples, (size_t)count * 4);
	EnterCriticalSection(&g_send_cs);
	send_frame(2, packet, HDR + count * 4);
	LeaveCriticalSection(&g_send_cs);
	free(samples);
	free(packet);
}

static void start_audio(void)
{
	char buf[384];
	if (!g_connected) return;
	_snprintf(buf, sizeof(buf),
		"AUDIO_STREAM_SAMPLE_TYPE:float32;"
		"AUDIO_STREAM_CHANNELS:1;"
		"AUDIO_SAMPLERATE:%d;"
		"AUDIO_STREAM_SAMPLES:%d;"
		"TX_STREAM_AUDIO_BUFFERING:100;"
		"AUDIO_START:%d;",
		g_tci_rate, samples_per_packet(g_tci_rate), g_rx_num);
	if (send_text(buf)) g_audio_on = 1;
}

static void stop_audio(void)
{
	char buf[64];
	if (!g_connected || !g_audio_on) { g_audio_on = 0; return; }
	_snprintf(buf, sizeof(buf), "AUDIO_STOP:%d;", g_rx_num);
	send_text(buf);
	g_audio_on = 0;
}

static void handle_command(const char *command)
{
	char name[64];
	const char *colon = strchr(command, ':');
	size_t nlen = colon ? (size_t)(colon - command) : strlen(command);
	size_t i;
	if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
	for (i = 0; i < nlen; i++) name[i] = (char)toupper((unsigned char)command[i]);
	name[nlen] = 0;
	if (strcmp(name, "READY") == 0) {
		g_ready = 1;
		if ((g_want_in || g_want_out) && !g_audio_on) start_audio();
		if (g_rx_event) SetEvent(g_rx_event);
	}
}

static void handle_text(const char *payload, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		if (g_text_n < (int)sizeof(g_text) - 1) g_text[g_text_n++] = payload[i];
		else { g_text_n = 0; }
		if (payload[i] == ';') {
			g_text[g_text_n - 1] = 0;
			if (g_text[0]) handle_command(g_text);
			g_text_n = 0;
		}
	}
}

static void push_rx_sample(double s)
{
	int cap = g_app_rate > 1000 ? g_app_rate * 2 : 22050;
	int idx;
	if (cap > RX_CAP) cap = RX_CAP;
	if (s > 32767.0) s = 32767.0;
	if (s < -32768.0) s = -32768.0;
	if (g_rx_count >= cap) {
		g_rx_head = (g_rx_head + 1) % RX_CAP;
		g_rx_count--;
	}
	idx = (g_rx_head + g_rx_count) % RX_CAP;
	g_rx[idx] = (short)s;
	g_rx_count++;
}

static void handle_binary(const unsigned char *data, int len)
{
	int format, count, kind, channels, bytes, offset, i;
	int available, frames;
	float tmp[256];
	int produced;
	if (len < HDR) return;
	format = (int)rd_u32(data + 8);
	count = (int)rd_u32(data + 20);
	kind = (int)rd_u32(data + 24);
	channels = (int)rd_u32(data + 28);
	if (channels <= 0) channels = 1;
	if (count <= 0 || count > 65536) return;
	if (kind == TX_CHRONO) {
		send_tx_audio(count);
		return;
	}
	if (kind != RX_AUDIO || !g_want_in) return;
	bytes = format == 0 ? 2 : format == 1 ? 3 : 4;
	available = (len - HDR) / bytes;
	if (count < available) available = count;
	frames = available / channels;
	offset = HDR;
	EnterCriticalSection(&g_audio_cs);
	for (i = 0; i < frames; i++) {
		float sum = 0;
		int ch;
		for (ch = 0; ch < channels; ch++) sum += read_sample(data, &offset, format);
		resamp_push(g_rx_src, &g_rx_src_n, 16384, sum / (float)channels);
	}
	for (;;) {
		produced = resamp_pull(g_rx_src, &g_rx_src_n, &g_rx_pos, g_rx_step, tmp, 256);
		if (produced <= 0) break;
		for (i = 0; i < produced; i++) push_rx_sample((double)tmp[i] * 32767.0);
	}
	LeaveCriticalSection(&g_audio_cs);
	if (g_rx_event) SetEvent(g_rx_event);
}

static int recv_some(unsigned char *buf, int len)
{
	int n = recv(g_sock, (char *)buf, len, 0);
	if (n == 0) return -1;
	if (n < 0) {
		int err = WSAGetLastError();
		if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) return 0;
		return -1;
	}
	return n;
}

static int read_exact(unsigned char *buf, int len)
{
	int got = 0;
	while (got < len && !g_stop) {
		int n = recv_some(buf + got, len - got);
		if (n < 0) return 0;
		if (n == 0) continue;
		got += n;
	}
	return got == len;
}

static int read_frame(void)
{
	unsigned char hdr[2], mask[4];
	unsigned int len;
	int opcode, fin, masked;
	unsigned char *payload = NULL;
	if (!read_exact(hdr, 2)) return -1;
	fin = (hdr[0] & 0x80) != 0;
	opcode = hdr[0] & 0x0f;
	masked = (hdr[1] & 0x80) != 0;
	len = hdr[1] & 0x7f;
	if (len == 126) {
		unsigned char ext[2];
		if (!read_exact(ext, 2)) return -1;
		len = ((unsigned int)ext[0] << 8) | ext[1];
	} else if (len == 127) return -1;
	if (masked && !read_exact(mask, 4)) return -1;
	if (len > 1048576) return -1;
	if (len) {
		unsigned int i;
		payload = (unsigned char *)malloc(len);
		if (!payload || !read_exact(payload, (int)len)) { free(payload); return -1; }
		if (masked) for (i = 0; i < len; i++) payload[i] = (unsigned char)(payload[i] ^ mask[i & 3]);
	}
	if (opcode == 8) { free(payload); return -1; }
	if (opcode == 9) {
		EnterCriticalSection(&g_send_cs);
		send_frame(10, payload, (int)len);
		LeaveCriticalSection(&g_send_cs);
		free(payload);
		return 0;
	}
	if (opcode == 10 || !len) { free(payload); return 0; }
	if (opcode == 0) {
		if (g_frag_op < 0) { free(payload); return 0; }
		if (g_frag_n + (int)len > g_frag_cap) {
			g_frag_cap = g_frag_n + (int)len + 1024;
			g_frag = (unsigned char *)realloc(g_frag, (size_t)g_frag_cap);
		}
		if (g_frag) { memcpy(g_frag + g_frag_n, payload, len); g_frag_n += (int)len; }
		free(payload);
		if (!fin) return 0;
		opcode = g_frag_op;
		payload = g_frag;
		len = (unsigned int)g_frag_n;
		g_frag = NULL;
		g_frag_n = g_frag_cap = 0;
		g_frag_op = -1;
	} else if (!fin && (opcode == 1 || opcode == 2)) {
		free(g_frag);
		g_frag = payload;
		g_frag_n = (int)len;
		g_frag_cap = (int)len;
		g_frag_op = opcode;
		return 0;
	}
	if (opcode == 1) handle_text((char *)payload, (int)len);
	else if (opcode == 2) handle_binary(payload, (int)len);
	free(payload);
	return 0;
}

static void set_timeout(int ms)
{
	DWORD t = (DWORD)ms;
	setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
	setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
}

static void ws_key(char *out, int outlen)
{
	static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned char raw[16];
	char tmp[32];
	int i, n = 0;
	for (i = 0; i < 16; i++) raw[i] = (unsigned char)(rand() & 255);
	for (i = 0; i < 15; i += 3) {
		unsigned v = (raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2];
		tmp[n++] = tbl[(v >> 18) & 63];
		tmp[n++] = tbl[(v >> 12) & 63];
		tmp[n++] = tbl[(v >> 6) & 63];
		tmp[n++] = tbl[v & 63];
	}
	{
		unsigned v = raw[15] << 16;
		tmp[n++] = tbl[(v >> 18) & 63];
		tmp[n++] = tbl[(v >> 12) & 63];
		tmp[n++] = '=';
		tmp[n++] = '=';
		tmp[n] = 0;
	}
	strncpy(out, tmp, (size_t)outlen - 1);
	out[outlen - 1] = 0;
}

static int handshake(void)
{
	unsigned long addr;
	struct sockaddr_in sa;
	struct hostent *he;
	char req[640], key[40], resp[8192];
	int resp_n = 0, one = 1;
	resp[0] = 0;
	addr = inet_addr(g_host);
	if (addr == INADDR_NONE) {
		he = gethostbyname(g_host);
		if (!he || !he->h_addr_list[0]) return 0;
		memcpy(&addr, he->h_addr_list[0], 4);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)g_port);
	sa.sin_addr.s_addr = addr;
	g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_sock == INVALID_SOCKET) return 0;
	if (connect(g_sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
		return 0;
	}
	setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	set_timeout(5000);
	ws_key(key, sizeof(key));
	_snprintf(req, sizeof(req),
		"GET / HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n",
		g_host, g_port, key);
	if (!send_all((unsigned char *)req, (int)strlen(req))) return 0;
	while (!strstr(resp, "\r\n\r\n") && resp_n < (int)sizeof(resp) - 1) {
		unsigned char tmp[512];
		int n = recv_some(tmp, sizeof(tmp));
		if (n < 0) return 0;
		if (n == 0) continue;
		if (resp_n + n >= (int)sizeof(resp)) n = (int)sizeof(resp) - 1 - resp_n;
		memcpy(resp + resp_n, tmp, (size_t)n);
		resp_n += n;
		resp[resp_n] = 0;
	}
	if (!strstr(resp, "101")) return 0;
	set_timeout(200);
	return 1;
}

static DWORD WINAPI thread_main(LPVOID unused)
{
	(void)unused;
	g_failed = 0;
	if (!g_wsa) {
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data) == 0) g_wsa = 1;
	}
	if (!handshake()) {
		g_failed = 1;
		g_connected = 0;
		if (g_rx_event) SetEvent(g_rx_event);
		if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
		return 0;
	}
	g_connected = 1;
	if (g_rx_event) SetEvent(g_rx_event);
	if (g_want_in || g_want_out) start_audio();
	while (!g_stop) {
		if (read_frame() < 0) break;
	}
	g_connected = 0;
	g_ready = 0;
	g_audio_on = 0;
	if (g_rx_event) SetEvent(g_rx_event);
	EnterCriticalSection(&g_send_cs);
	if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
	LeaveCriticalSection(&g_send_cs);
	return 0;
}

static void shutdown_link(void)
{
	HANDLE th = g_thread;
	g_stop = 1;
	if (g_rx_event) SetEvent(g_rx_event);
	EnterCriticalSection(&g_send_cs);
	if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
	LeaveCriticalSection(&g_send_cs);
	if (th) {
		WaitForSingleObject(th, 3000);
		CloseHandle(th);
		g_thread = NULL;
	}
	g_stop = 0;
	g_connected = 0;
	g_ready = 0;
	g_failed = 0;
	g_audio_on = 0;
	g_ptt = 0;
	EnterCriticalSection(&g_audio_cs);
	g_rx_count = 0;
	g_rx_head = 0;
	g_tx_count = 0;
	g_tx_head = 0;
	LeaveCriticalSection(&g_audio_cs);
	g_text_n = 0;
	free(g_frag);
	g_frag = NULL;
	g_frag_n = g_frag_cap = 0;
	g_frag_op = -1;
}

static int ensure(int rate)
{
	DWORD t0, code;
	if (rate < 1000) rate = 11025;
	g_app_rate = rate;
	g_tci_rate = pick_rate(rate);
	if (g_connected && g_ready) return 1;
	if (g_thread) {
		code = 0;
		if (GetExitCodeThread(g_thread, &code) && code != STILL_ACTIVE) {
			CloseHandle(g_thread);
			g_thread = NULL;
		}
	}
	if (!g_thread) {
		DWORD id = 0;
		g_failed = 0;
		g_stop = 0;
		g_connected = 0;
		g_ready = 0;
		g_thread = CreateThread(NULL, 0, thread_main, NULL, 0, &id);
		if (!g_thread) return 0;
	}
	t0 = GetTickCount();
	while (GetTickCount() - t0 < 4000) {
		if (g_ready) return 1;
		if (g_failed) return 0;
		Sleep(20);
	}
	return g_connected ? 1 : 0;
}

__declspec(dllexport) void __stdcall mmwPumpMessages(void) {}

__declspec(dllexport) LONG __stdcall mmwGetTimeout(void) { return 5000; }

__declspec(dllexport) void __stdcall mmwSetPTT(LONG tx)
{
	char buf[64];
	g_ptt = tx ? 1 : 0;
	if (!g_connected || !g_want_out) return;
	_snprintf(buf, sizeof(buf), "TRX:%d,%s%s;", g_rx_num, tx ? "true" : "false", tx ? ",tci" : "");
	send_text(buf);
}

__declspec(dllexport) LONG __stdcall mmwInOpen(DWORD sampfreq, DWORD size)
{
	load_config();
	g_want_in = 1;
	g_block = size ? (int)size : 2048;
	EnterCriticalSection(&g_audio_cs);
	g_rx_count = 0;
	g_rx_head = 0;
	resamp_reset(g_rx_src, &g_rx_src_n, &g_rx_pos, &g_rx_step, pick_rate((int)sampfreq), sampfreq ? sampfreq : 11025);
	LeaveCriticalSection(&g_audio_cs);
	if (g_rx_event) ResetEvent(g_rx_event);
	if (!ensure((int)sampfreq)) { g_want_in = 0; return 0; }
	if (!g_audio_on) start_audio();
	return 1;
}

__declspec(dllexport) void __stdcall mmwInClose(void)
{
	g_want_in = 0;
	if (!g_want_out) stop_audio();
	if (g_rx_event) SetEvent(g_rx_event);
}

__declspec(dllexport) BOOL __stdcall mmwInRead(SHORT *buf)
{
	DWORD t0;
	int need = g_block > 0 ? g_block : 2048;
	if (!buf || !g_want_in) return FALSE;
	t0 = GetTickCount();
	for (;;) {
		EnterCriticalSection(&g_audio_cs);
		if (g_rx_count >= need) {
			int i;
			for (i = 0; i < need; i++) {
				buf[i] = g_rx[g_rx_head];
				g_rx_head = (g_rx_head + 1) % RX_CAP;
			}
			g_rx_count -= need;
			if (g_rx_count < need && g_rx_event) ResetEvent(g_rx_event);
			LeaveCriticalSection(&g_audio_cs);
			return TRUE;
		}
		LeaveCriticalSection(&g_audio_cs);
		if (g_failed && !g_connected) return FALSE;
		if (!g_connected && GetTickCount() - t0 > 1500) return FALSE;
		if (GetTickCount() - t0 > 5000) return FALSE;
		if (g_stop) return FALSE;
		WaitForSingleObject(g_rx_event, 100);
	}
}

__declspec(dllexport) LONG __stdcall mmwGetInExist(void)
{
	LONG n;
	EnterCriticalSection(&g_audio_cs);
	n = g_block > 0 ? g_rx_count / g_block : 0;
	LeaveCriticalSection(&g_audio_cs);
	return n;
}

__declspec(dllexport) BOOL __stdcall mmwIsInCritical(void)
{
	BOOL n;
	EnterCriticalSection(&g_audio_cs);
	n = g_rx_count >= (g_block > 0 ? g_block * 4 : 8192);
	LeaveCriticalSection(&g_audio_cs);
	return n;
}

__declspec(dllexport) LONG __stdcall mmwOutOpen(DWORD sampfreq, DWORD size)
{
	load_config();
	if (tci_output_is_card()) {
		g_want_out = 1;
		if (size) g_block = (int)size;
		return tci_card_open(sampfreq, size);
	}
	g_want_out = 1;
	g_ptt = 0;
	g_bcc = 0x7fffffff;
	g_bcc_frac = 0;
	if (size) g_block = (int)size;
	EnterCriticalSection(&g_audio_cs);
	g_tx_count = 0;
	g_tx_head = 0;
	resamp_reset(g_tx_src, &g_tx_src_n, &g_tx_pos, &g_tx_step, sampfreq ? sampfreq : 11025, pick_rate((int)sampfreq));
	LeaveCriticalSection(&g_audio_cs);
	if (!ensure((int)sampfreq)) { g_want_out = 0; return 0; }
	if (!g_audio_on) start_audio();
	return 1;
}

__declspec(dllexport) void __stdcall mmwOutAbort(void)
{
	char buf[64];
	if (tci_output_is_card()) {
		tci_card_abort();
		g_want_out = 0;
		g_ptt = 0;
		return;
	}
	g_want_out = 0;
	g_ptt = 0;
	if (g_connected) {
		_snprintf(buf, sizeof(buf), "TRX:%d,false;", g_rx_num);
		send_text(buf);
	}
	if (!g_want_in) stop_audio();
	EnterCriticalSection(&g_audio_cs);
	g_tx_count = 0;
	g_tx_head = 0;
	LeaveCriticalSection(&g_audio_cs);
	g_bcc = 0x7fffffff;
}

__declspec(dllexport) void __stdcall mmwOutFlush(void) {}

static void feed_block(const SHORT *buf, int need);

__declspec(dllexport) BOOL __stdcall mmwOutWrite(const SHORT *buf)
{
	int need;
	DWORD t0;
	int cap;
	if (!buf || !g_want_out) return FALSE;
	need = g_block > 0 ? g_block : 2048;
	if (tci_output_is_card()) return tci_card_write(buf, need);
	feed_block(buf, need);
	cap = g_tci_rate > 1000 ? g_tci_rate * 3 : 36000;
	t0 = GetTickCount();
	for (;;) {
		EnterCriticalSection(&g_audio_cs);
		if (g_tx_count <= cap || !g_want_out) {
			LeaveCriticalSection(&g_audio_cs);
			return g_want_out ? TRUE : FALSE;
		}
		if (GetTickCount() - t0 > 2000) {
			int drop = g_tx_count - cap / 2;
			if (drop < 0) drop = 0;
			g_tx_head = (g_tx_head + drop) % TX_CAP;
			g_tx_count -= drop;
			note_sent(drop);
			LeaveCriticalSection(&g_audio_cs);
			return TRUE;
		}
		LeaveCriticalSection(&g_audio_cs);
		Sleep(5);
	}
}

__declspec(dllexport) BOOL __stdcall mmwIsOutCritical(void)
{
	BOOL n;
	EnterCriticalSection(&g_audio_cs);
	n = g_block > 0 ? (g_tx_count / g_block) <= 2 : TRUE;
	LeaveCriticalSection(&g_audio_cs);
	return n;
}

__declspec(dllexport) BOOL __stdcall mmwIsOutFull(void)
{
	BOOL n;
	int limit = g_tci_rate > 1000 ? g_tci_rate : 12000;
	EnterCriticalSection(&g_audio_cs);
	n = g_tx_count >= limit;
	LeaveCriticalSection(&g_audio_cs);
	return n;
}

__declspec(dllexport) LONG __stdcall mmwGetOutRemaining(void)
{
	LONG n;
	EnterCriticalSection(&g_audio_cs);
	n = g_block > 0 ? g_tx_count / g_block : 0;
	LeaveCriticalSection(&g_audio_cs);
	return n;
}

static void feed_block(const SHORT *buf, int need)
{
	int i, produced;
	float tmp[256];
	EnterCriticalSection(&g_audio_cs);
	for (i = 0; i < need; i++)
		resamp_push(g_tx_src, &g_tx_src_n, 16384, buf[i] / 32767.f);
	for (;;) {
		produced = resamp_pull(g_tx_src, &g_tx_src_n, &g_tx_pos, g_tx_step, tmp, 256);
		if (produced <= 0) break;
		for (i = 0; i < produced; i++) {
			int idx;
			if (g_tx_count >= TX_CAP) {
				g_tx_head = (g_tx_head + 1) % TX_CAP;
				g_tx_count--;
			}
			idx = (g_tx_head + g_tx_count) % TX_CAP;
			g_tx[idx] = tmp[i];
			g_tx_count++;
		}
	}
	LeaveCriticalSection(&g_audio_cs);
}

__declspec(dllexport) void __stdcall tci_feed_tx(const SHORT *buf, int count)
{
	if (buf && count > 0) feed_block(buf, count);
}

__declspec(dllexport) void __stdcall tci_tx_begin(int rate)
{
	load_config();
	g_want_out = 1;
	g_ptt = 1;
	if (rate > 1000) g_app_rate = rate;
	EnterCriticalSection(&g_audio_cs);
	g_tx_count = 0;
	g_tx_head = 0;
	resamp_reset(g_tx_src, &g_tx_src_n, &g_tx_pos, &g_tx_step, g_app_rate, pick_rate(g_app_rate));
	LeaveCriticalSection(&g_audio_cs);
	if (!ensure(g_app_rate)) return;
	if (!g_audio_on) start_audio();
	mmwSetPTT(1);
}

__declspec(dllexport) void __stdcall tci_tx_end(void)
{
	mmwSetPTT(0);
}

__declspec(dllexport) LONG __stdcall mmwGetOutCounter(void) { return g_bcc; }

__declspec(dllexport) void __stdcall mmwSetOutCounter(LONG count)
{
	g_bcc = (int)count;
	g_bcc_frac = 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		InitializeCriticalSection(&g_send_cs);
		InitializeCriticalSection(&g_audio_cs);
		g_rx_event = CreateEventA(NULL, TRUE, FALSE, NULL);
		CreateThread(NULL, 0, tci_install_hooks, NULL, 0, NULL);
	} else if (reason == DLL_PROCESS_DETACH) {
		shutdown_link();
		if (g_rx_event) CloseHandle(g_rx_event);
		DeleteCriticalSection(&g_send_cs);
		DeleteCriticalSection(&g_audio_cs);
		if (g_wsa) WSACleanup();
	}
	return TRUE;
}
