//Copyright+LGPL
//-----------------------------------------------------------------------------------------------------------------------------------------------
// TCI audio device for MMSSTV / YONIQ.
//
// Speaks the Expert Electronics Transceiver Control Interface: a WebSocket
// on top of TCP. Receiver audio arrives as RX_AUDIO_STREAM binary frames and
// is resampled to the MMSSTV sample rate. Transmit audio is queued and sent
// as TX_AUDIO_STREAM when the server asks with TX_CHRONO. PTT is TRX.
//-----------------------------------------------------------------------------------------------------------------------------------------------
#include <vcl.h>
#pragma hdrstop

#include "TciAudio.h"

#include <deque>
#include <string>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma link "ws2_32.lib"

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x0001
#endif
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 0x1006
#endif
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 0x1005
#endif

#define TCI_HDR_BYTES 64
#define TCI_RX_AUDIO  1
#define TCI_TX_AUDIO  2
#define TCI_TX_CHRONO 3
#define TCI_FMT_F32   3

int __fastcall IsTciDeviceName(const char *name)
{
	if( name == NULL || name[0] == 0 ) return 0;
	while( *name == ' ' ) name++;
	return !_stricmp(name, "TCI");
}

static int PickTciRate(int app)
{
	static const int rates[4] = {8000, 12000, 24000, 48000};
	int best = 12000;
	int bestDiff = 100000000;
	int i;
	if( app < 1000 ) app = 11025;
	for( i = 0; i < 4; i++ ){
		int d = app - rates[i];
		if( d < 0 ) d = -d;
		if( d < bestDiff ){
			bestDiff = d;
			best = rates[i];
		}
	}
	return best;
}

static int SamplesPerPacket(int rate)
{
	if( rate <= 8000 ) return 128;
	if( rate <= 12000 ) return 256;
	if( rate <= 24000 ) return 512;
	return 1024;
}

struct Resampler
{
	std::deque<float> src;
	double pos;
	double step;

	void Reset(double srcRate, double dstRate)
	{
		src.clear();
		pos = 0;
		if( dstRate < 1.0 ) dstRate = 1.0;
		if( srcRate < 1.0 ) srcRate = dstRate;
		step = srcRate / dstRate;
		if( step < 0.0001 ) step = 0.0001;
	}

	void Push(float s)
	{
		src.push_back(s);
	}

	int Pull(float *out, int maxn)
	{
		int n = 0;
		while( n < maxn && (pos + 1.0) < (double)src.size() ){
			size_t i = (size_t)pos;
			double frac = pos - (double)i;
			out[n++] = (float)(src[i] * (1.0 - frac) + src[i + 1] * frac);
			pos += step;
		}
		size_t drop = (size_t)pos;
		if( drop > 0 && !src.empty() ){
			if( drop >= src.size() ){
				float last = src.back();
				src.clear();
				src.push_back(last);
				pos = 0;
			}
			else {
				src.erase(src.begin(), src.begin() + drop);
				pos -= (double)drop;
			}
		}
		return n;
	}
};

struct CTciAudio::Impl
{
	CRITICAL_SECTION csSend;
	CRITICAL_SECTION csAudio;
	HANDLE thread;
	HANDLE rxEvent;
	SOCKET sock;
	volatile LONG stop;
	volatile LONG connected;
	volatile LONG ready;
	volatile LONG failed;
	volatile LONG audioOn;
	volatile LONG wantIn;
	volatile LONG wantOut;
	volatile LONG ptt;
	int wsa;
	char host[256];
	int port;
	int receiver;
	int appRate;
	int tciRate;
	int block;
	int bcc;
	int bccFrac;
	Resampler rxRes;
	Resampler txRes;
	std::deque<double> rx;
	std::deque<float> tx;
	std::vector<unsigned char> frag;
	int fragOp;
	std::string text;

	Impl()
	{
		memset(host, 0, sizeof(host));
		strcpy(host, "127.0.0.1");
		port = 40001;
		receiver = 0;
		appRate = 11025;
		tciRate = 12000;
		block = 2048;
		bcc = 0x7fffffff;
		bccFrac = 0;
		thread = NULL;
		rxEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		sock = INVALID_SOCKET;
		stop = 0;
		connected = 0;
		ready = 0;
		failed = 0;
		audioOn = 0;
		wantIn = 0;
		wantOut = 0;
		ptt = 0;
		wsa = 0;
		fragOp = -1;
		InitializeCriticalSection(&csSend);
		InitializeCriticalSection(&csAudio);
		rxRes.Reset(12000, 11025);
		txRes.Reset(11025, 12000);
	}

	~Impl()
	{
		Shutdown();
		DeleteCriticalSection(&csSend);
		DeleteCriticalSection(&csAudio);
		if( rxEvent ) CloseHandle(rxEvent);
		if( wsa ) WSACleanup();
	}

	void WakeRx(void)
	{
		if( rxEvent ) SetEvent(rxEvent);
	}

	int SendAll(const unsigned char *data, int len)
	{
		int sent = 0;
		while( sent < len ){
			int n = send(sock, (const char *)data + sent, len - sent, 0);
			if( n <= 0 ) return 0;
			sent += n;
		}
		return 1;
	}

	int SendFrame(int opcode, const unsigned char *data, int len)
	{
		unsigned char mask[4];
		unsigned char hdr[8];
		int hlen = 2;
		int i;
		std::vector<unsigned char> packet;
		if( sock == INVALID_SOCKET ) return 0;
		for( i = 0; i < 4; i++ ) mask[i] = (unsigned char)(rand() & 0xff);
		hdr[0] = (unsigned char)(0x80 | opcode);
		if( len < 126 ){
			hdr[1] = (unsigned char)(0x80 | len);
		}
		else {
			hdr[1] = (unsigned char)(0x80 | 126);
			hdr[2] = (unsigned char)((len >> 8) & 0xff);
			hdr[3] = (unsigned char)(len & 0xff);
			hlen = 4;
		}
		memcpy(hdr + hlen, mask, 4);
		hlen += 4;
		packet.resize(hlen + len);
		memcpy(&packet[0], hdr, hlen);
		for( i = 0; i < len; i++ ){
			unsigned char b = data ? data[i] : 0;
			packet[hlen + i] = (unsigned char)(b ^ mask[i & 3]);
		}
		return SendAll(&packet[0], (int)packet.size());
	}

	int SendText(const char *command)
	{
		int ok;
		EnterCriticalSection(&csSend);
		ok = SendFrame(1, (const unsigned char *)command, (int)strlen(command));
		LeaveCriticalSection(&csSend);
		return ok;
	}

	void WriteU32(unsigned char *p, unsigned int v)
	{
		memcpy(p, &v, 4);
	}

	unsigned int ReadU32(const unsigned char *p)
	{
		unsigned int v;
		memcpy(&v, p, 4);
		return v;
	}

	float ReadSample(const unsigned char *data, size_t &offset, int format)
	{
		if( format == 0 ){
			short v;
			memcpy(&v, data + offset, 2);
			offset += 2;
			return v / 32768.f;
		}
		if( format == 1 ){
			int v = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16);
			if( v & 0x800000 ) v |= ~0xFFFFFF;
			offset += 3;
			return v / 8388608.f;
		}
		if( format == 2 ){
			int v;
			memcpy(&v, data + offset, 4);
			offset += 4;
			return v / 2147483648.f;
		}
		float v;
		memcpy(&v, data + offset, 4);
		offset += 4;
		return v;
	}

	void NoteSent(int samples)
	{
		if( bcc > 0x100000 || block < 1 || samples < 1 ) return;
		bccFrac += samples;
		while( bccFrac >= block && bcc > -1 ){
			bccFrac -= block;
			bcc--;
		}
	}

	void SendTxAudio(int count)
	{
		int i;
		int real = 0;
		std::vector<unsigned char> packet;
		std::vector<float> samples;
		if( count < 1 ) count = 1;
		if( count > 8192 ) count = 8192;
		samples.resize(count);
		EnterCriticalSection(&csAudio);
		if( ptt ){
			for( i = 0; i < count; i++ ){
				if( tx.empty() ) break;
				samples[i] = tx.front();
				tx.pop_front();
				real++;
			}
			NoteSent(real);
		}
		LeaveCriticalSection(&csAudio);
		for( i = real; i < count; i++ ) samples[i] = 0.f;
		packet.resize(TCI_HDR_BYTES + count * 4);
		memset(&packet[0], 0, packet.size());
		WriteU32(&packet[0], (unsigned int)receiver);
		WriteU32(&packet[4], (unsigned int)tciRate);
		WriteU32(&packet[8], TCI_FMT_F32);
		WriteU32(&packet[20], (unsigned int)count);
		WriteU32(&packet[24], TCI_TX_AUDIO);
		WriteU32(&packet[28], 1);
		memcpy(&packet[TCI_HDR_BYTES], &samples[0], count * 4);
		EnterCriticalSection(&csSend);
		SendFrame(2, &packet[0], (int)packet.size());
		LeaveCriticalSection(&csSend);
	}

	void StartAudio(void)
	{
		char buf[384];
		int rate = tciRate;
		int rxn = receiver;
		if( rxn < 0 ) rxn = 0;
		if( rxn > 7 ) rxn = 7;
		if( !connected ) return;
		sprintf(buf,
			"AUDIO_STREAM_SAMPLE_TYPE:float32;"
			"AUDIO_STREAM_CHANNELS:1;"
			"AUDIO_SAMPLERATE:%d;"
			"AUDIO_STREAM_SAMPLES:%d;"
			"TX_STREAM_AUDIO_BUFFERING:100;"
			"AUDIO_START:%d;",
			rate, SamplesPerPacket(rate), rxn);
		if( SendText(buf) ) audioOn = 1;
	}

	void StopAudio(void)
	{
		char buf[64];
		if( !connected || !audioOn ) {
			audioOn = 0;
			return;
		}
		sprintf(buf, "AUDIO_STOP:%d;", receiver);
		SendText(buf);
		audioOn = 0;
	}

	void HandleCommand(const std::string &command)
	{
		std::string name, args;
		size_t colon = command.find(':');
		if( colon == std::string::npos ) name = command;
		else {
			name = command.substr(0, colon);
			args = command.substr(colon + 1);
		}
		for( size_t i = 0; i < name.size(); i++ )
			name[i] = (char)toupper((unsigned char)name[i]);
		if( name == "READY" ){
			ready = 1;
			if( (wantIn || wantOut) && !audioOn ) StartAudio();
			WakeRx();
		}
	}

	void HandleText(const std::string &payload)
	{
		text.append(payload);
		if( text.size() > 65536 ) text.clear();
		for( ;; ){
			size_t end = text.find(';');
			if( end == std::string::npos ) break;
			std::string command = text.substr(0, end);
			text.erase(0, end + 1);
			if( !command.empty() ) HandleCommand(command);
		}
	}

	void HandleBinary(const unsigned char *data, int len)
	{
		int format, count, kind, channels, bytes, i;
		size_t available, samples, frames, offset;
		size_t cap;
		float tmp[1024];
		int produced;
		if( len < TCI_HDR_BYTES ) return;
		format = (int)ReadU32(data + 8);
		count = (int)ReadU32(data + 20);
		kind = (int)ReadU32(data + 24);
		channels = (int)ReadU32(data + 28);
		if( channels <= 0 ) channels = 1;
		if( count <= 0 || count > 65536 ) return;
		if( kind == TCI_TX_CHRONO ){
			int rate = (int)ReadU32(data + 4);
			SendTxAudio(count);
			(void)rate;
			return;
		}
		if( kind != TCI_RX_AUDIO || !wantIn ) return;
		bytes = (format == 0) ? 2 : (format == 1) ? 3 : 4;
		available = (size_t)(len - TCI_HDR_BYTES) / (size_t)bytes;
		samples = (size_t)count;
		if( samples > available ) samples = available;
		frames = samples / (size_t)channels;
		offset = TCI_HDR_BYTES;
		EnterCriticalSection(&csAudio);
		cap = (size_t)((appRate > 1000 ? appRate : 11025) * 2);
		for( size_t frame = 0; frame < frames; frame++ ){
			float sum = 0;
			for( i = 0; i < channels; i++ )
				sum += ReadSample(data, offset, format);
			rxRes.Push(sum / (float)channels);
		}
		for( ;; ){
			produced = rxRes.Pull(tmp, 1024);
			if( produced <= 0 ) break;
			for( i = 0; i < produced; i++ ){
				double s = (double)tmp[i] * 32767.0;
				if( s > 32767.0 ) s = 32767.0;
				if( s < -32768.0 ) s = -32768.0;
				rx.push_back(s);
			}
		}
		while( rx.size() > cap ) rx.pop_front();
		LeaveCriticalSection(&csAudio);
		WakeRx();
	}

	int RecvSome(unsigned char *buf, int len)
	{
		int n = recv(sock, (char *)buf, len, 0);
		if( n == 0 ) return -1;
		if( n < 0 ){
			int err = WSAGetLastError();
			if( err == WSAETIMEDOUT || err == WSAEWOULDBLOCK ) return 0;
			return -1;
		}
		return n;
	}

	int ReadExact(unsigned char *buf, int len)
	{
		int got = 0;
		while( got < len && !stop ){
			int n = RecvSome(buf + got, len - got);
			if( n < 0 ) return 0;
			if( n == 0 ) continue;
			got += n;
		}
		return got == len;
	}

	int ReadFrame(void)
	{
		unsigned char hdr[2];
		unsigned char mask[4];
		unsigned int len;
		int masked;
		int opcode;
		int fin;
		std::vector<unsigned char> payload;
		if( !ReadExact(hdr, 2) ) return -1;
		fin = (hdr[0] & 0x80) != 0;
		opcode = hdr[0] & 0x0f;
		masked = (hdr[1] & 0x80) != 0;
		len = hdr[1] & 0x7f;
		if( len == 126 ){
			unsigned char ext[2];
			if( !ReadExact(ext, 2) ) return -1;
			len = ((unsigned int)ext[0] << 8) | ext[1];
		}
		else if( len == 127 ){
			return -1;
		}
		if( masked && !ReadExact(mask, 4) ) return -1;
		if( len > 1048576 ) return -1;
		if( len ){
			payload.resize(len);
			if( !ReadExact(&payload[0], (int)len) ) return -1;
			if( masked ){
				for( unsigned int i = 0; i < len; i++ )
					payload[i] = (unsigned char)(payload[i] ^ mask[i & 3]);
			}
		}
		if( opcode == 8 ) return -1;
		if( opcode == 9 ){
			EnterCriticalSection(&csSend);
			SendFrame(10, payload.empty() ? NULL : &payload[0], (int)payload.size());
			LeaveCriticalSection(&csSend);
			return 0;
		}
		if( opcode == 10 || payload.empty() ) return 0;
		if( opcode == 0 ){
			if( fragOp < 0 ) return 0;
			frag.insert(frag.end(), payload.begin(), payload.end());
			if( !fin ) return 0;
			opcode = fragOp;
			payload.swap(frag);
			frag.clear();
			fragOp = -1;
		}
		else if( !fin && (opcode == 1 || opcode == 2) ){
			frag.swap(payload);
			fragOp = opcode;
			return 0;
		}
		if( opcode == 1 )
			HandleText(std::string((char *)&payload[0], payload.size()));
		else if( opcode == 2 )
			HandleBinary(&payload[0], (int)payload.size());
		return 0;
	}

	void SetTimeout(int ms)
	{
		DWORD t = (DWORD)ms;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
		setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
	}

	std::string WsKey(void)
	{
		static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		unsigned char raw[16];
		std::string out;
		int i;
		for( i = 0; i < 16; i++ ) raw[i] = (unsigned char)(rand() & 0xff);
		for( i = 0; i < 15; i += 3 ){
			unsigned n = (raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2];
			out.push_back(tbl[(n >> 18) & 63]);
			out.push_back(tbl[(n >> 12) & 63]);
			out.push_back(tbl[(n >> 6) & 63]);
			out.push_back(tbl[n & 63]);
		}
		{
			unsigned n = (raw[15] << 16);
			out.push_back(tbl[(n >> 18) & 63]);
			out.push_back(tbl[(n >> 12) & 63]);
			out += "==";
		}
		return out;
	}

	int Handshake(void)
	{
		unsigned long addr;
		struct sockaddr_in sa;
		struct hostent *he;
		char req[512];
		std::string resp;
		unsigned char tmp[512];
		int one = 1;
		const char *name = host[0] ? host : "127.0.0.1";

		addr = inet_addr(name);
		if( addr == INADDR_NONE ){
			he = gethostbyname(name);
			if( he == NULL || he->h_addr_list[0] == NULL ) return 0;
			memcpy(&addr, he->h_addr_list[0], 4);
		}
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((unsigned short)port);
		sa.sin_addr.s_addr = addr;
		sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if( sock == INVALID_SOCKET ) return 0;
		if( connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0 ){
			closesocket(sock);
			sock = INVALID_SOCKET;
			return 0;
		}
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
		SetTimeout(5000);
		sprintf(req,
			"GET / HTTP/1.1\r\n"
			"Host: %s:%d\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Key: %s\r\n"
			"Sec-WebSocket-Version: 13\r\n\r\n",
			name, port, WsKey().c_str());
		if( !SendAll((const unsigned char *)req, (int)strlen(req)) ) return 0;
		while( resp.find("\r\n\r\n") == std::string::npos && resp.size() < 8192 ){
			int n = RecvSome(tmp, sizeof(tmp));
			if( n < 0 ) return 0;
			if( n == 0 ) continue;
			resp.append((char *)tmp, n);
		}
		if( resp.find("101") == std::string::npos ) return 0;
		SetTimeout(200);
		return 1;
	}

	void ThreadMain(void)
	{
		failed = 0;
		if( !wsa ){
			WSADATA data;
			if( WSAStartup(MAKEWORD(1, 1), &data) == 0 ) wsa = 1;
		}
		if( !Handshake() ){
			failed = 1;
			connected = 0;
			WakeRx();
			if( sock != INVALID_SOCKET ){
				closesocket(sock);
				sock = INVALID_SOCKET;
			}
			return;
		}
		connected = 1;
		WakeRx();
		if( wantIn || wantOut ) StartAudio();
		while( !stop ){
			if( ReadFrame() < 0 ) break;
		}
		connected = 0;
		ready = 0;
		audioOn = 0;
		WakeRx();
		EnterCriticalSection(&csSend);
		if( sock != INVALID_SOCKET ){
			closesocket(sock);
			sock = INVALID_SOCKET;
		}
		LeaveCriticalSection(&csSend);
	}

	void Shutdown(void)
	{
		HANDLE th = thread;
		stop = 1;
		WakeRx();
		EnterCriticalSection(&csSend);
		if( sock != INVALID_SOCKET ){
			closesocket(sock);
			sock = INVALID_SOCKET;
		}
		LeaveCriticalSection(&csSend);
		if( th ){
			WaitForSingleObject(th, 3000);
			CloseHandle(th);
			thread = NULL;
		}
		stop = 0;
		connected = 0;
		ready = 0;
		failed = 0;
		audioOn = 0;
		ptt = 0;
		EnterCriticalSection(&csAudio);
		rx.clear();
		tx.clear();
		rxRes.Reset(tciRate, appRate);
		txRes.Reset(appRate, tciRate);
		LeaveCriticalSection(&csAudio);
		text.clear();
		frag.clear();
		fragOp = -1;
	}

	BOOL Ensure(int rate, int blk)
	{
		DWORD t0;
		if( rate < 1000 ) rate = 11025;
		if( blk < 1 ) blk = 2048;
		appRate = rate;
		block = blk;
		tciRate = PickTciRate(rate);
		if( connected && ready ) return TRUE;
		if( thread != NULL ){
			DWORD code = 0;
			if( GetExitCodeThread(thread, &code) && code != STILL_ACTIVE ){
				CloseHandle(thread);
				thread = NULL;
			}
		}
		if( thread == NULL ){
			DWORD id = 0;
			failed = 0;
			stop = 0;
			connected = 0;
			ready = 0;
			thread = CreateThread(NULL, 0, ThreadThunk, this, 0, &id);
			if( thread == NULL ) return FALSE;
		}
		t0 = GetTickCount();
		while( (GetTickCount() - t0) < 4000 ){
			if( ready ) return TRUE;
			if( failed ) return FALSE;
			Sleep(20);
		}
		return connected ? TRUE : FALSE;
	}

	static DWORD WINAPI ThreadThunk(LPVOID arg)
	{
		((Impl *)arg)->ThreadMain();
		return 0;
	}
};

CTciAudio TciAudio;

__fastcall CTciAudio::CTciAudio()
{
	m_impl = new Impl();
}

__fastcall CTciAudio::~CTciAudio()
{
	delete m_impl;
	m_impl = NULL;
}

void __fastcall CTciAudio::Configure(const char *host, int port, int receiver)
{
	Impl *d = m_impl;
	char next[256];
	int changed;
	if( host == NULL || host[0] == 0 ) host = "127.0.0.1";
	strncpy(next, host, sizeof(next) - 1);
	next[sizeof(next) - 1] = 0;
	if( port < 1 || port > 65535 ) port = 40001;
	if( receiver < 0 ) receiver = 0;
	if( receiver > 7 ) receiver = 7;
	changed = strcmp(d->host, next) || d->port != port || d->receiver != receiver;
	strcpy(d->host, next);
	d->port = port;
	d->receiver = receiver;
	if( changed && (d->connected || d->thread) ) d->Shutdown();
}

void __fastcall CTciAudio::Shutdown(void)
{
	m_impl->Shutdown();
}

BOOL __fastcall CTciAudio::InOpen(int rate, int block)
{
	Impl *d = m_impl;
	d->wantIn = 1;
	EnterCriticalSection(&d->csAudio);
	d->rx.clear();
	d->rxRes.Reset(PickTciRate(rate), rate > 0 ? rate : 11025);
	LeaveCriticalSection(&d->csAudio);
	ResetEvent(d->rxEvent);
	if( !d->Ensure(rate, block) ){
		d->wantIn = 0;
		return FALSE;
	}
	if( !d->audioOn ) d->StartAudio();
	return TRUE;
}

void __fastcall CTciAudio::InClose(void)
{
	Impl *d = m_impl;
	d->wantIn = 0;
	if( !d->wantOut ) d->StopAudio();
	if( d->rxEvent ) SetEvent(d->rxEvent);
}

BOOL __fastcall CTciAudio::InRead(double *p, int len)
{
	Impl *d = m_impl;
	DWORD t0 = GetTickCount();
	if( !d->wantIn ) return FALSE;
	while( !d->stop ){
		int have;
		EnterCriticalSection(&d->csAudio);
		have = (int)d->rx.size();
		if( have >= len ){
			for( int i = 0; i < len; i++ ){
				p[i] = d->rx.front();
				d->rx.pop_front();
			}
			if( (int)d->rx.size() < len ) ResetEvent(d->rxEvent);
			LeaveCriticalSection(&d->csAudio);
			return TRUE;
		}
		LeaveCriticalSection(&d->csAudio);
		if( d->failed && !d->connected ) return FALSE;
		if( !d->connected && (GetTickCount() - t0) > 1500 ) return FALSE;
		if( (GetTickCount() - t0) > 5000 ) return FALSE;
		WaitForSingleObject(d->rxEvent, 100);
	}
	return FALSE;
}

int __fastcall CTciAudio::GetInBC(void)
{
	Impl *d = m_impl;
	int n;
	EnterCriticalSection(&d->csAudio);
	n = d->block > 0 ? (int)d->rx.size() / d->block : 0;
	LeaveCriticalSection(&d->csAudio);
	return n;
}

int __fastcall CTciAudio::IsInCritical(void)
{
	Impl *d = m_impl;
	int n;
	EnterCriticalSection(&d->csAudio);
	n = (d->rx.size() >= (size_t)(d->block > 0 ? d->block * 4 : 8192)) ? 1 : 0;
	LeaveCriticalSection(&d->csAudio);
	return n;
}

BOOL __fastcall CTciAudio::OutOpen(int rate, int block)
{
	Impl *d = m_impl;
	d->wantOut = 1;
	d->ptt = 0;
	d->bcc = 0x7fffffff;
	d->bccFrac = 0;
	EnterCriticalSection(&d->csAudio);
	d->tx.clear();
	d->txRes.Reset(rate > 0 ? rate : 11025, PickTciRate(rate));
	LeaveCriticalSection(&d->csAudio);
	if( !d->Ensure(rate, block) ){
		d->wantOut = 0;
		return FALSE;
	}
	if( !d->audioOn ) d->StartAudio();
	return TRUE;
}

void __fastcall CTciAudio::OutAbort(void)
{
	Impl *d = m_impl;
	char buf[64];
	d->wantOut = 0;
	d->ptt = 0;
	if( d->connected ){
		sprintf(buf, "TRX:%d,false;", d->receiver);
		d->SendText(buf);
	}
	if( !d->wantIn ) d->StopAudio();
	EnterCriticalSection(&d->csAudio);
	d->tx.clear();
	LeaveCriticalSection(&d->csAudio);
	d->bcc = 0x7fffffff;
}

BOOL __fastcall CTciAudio::OutWrite(double *p, int len)
{
	Impl *d = m_impl;
	float tmp[1024];
	int i, produced;
	size_t cap;
	DWORD t0;
	if( !d->wantOut ) return FALSE;
	EnterCriticalSection(&d->csAudio);
	for( i = 0; i < len; i++ ){
		double s = p[i] / 32767.0;
		if( s > 1.0 ) s = 1.0;
		if( s < -1.0 ) s = -1.0;
		d->txRes.Push((float)s);
	}
	for( ;; ){
		produced = d->txRes.Pull(tmp, 1024);
		if( produced <= 0 ) break;
		for( i = 0; i < produced; i++ ) d->tx.push_back(tmp[i]);
	}
	LeaveCriticalSection(&d->csAudio);

	cap = (size_t)((d->tciRate > 1000 ? d->tciRate : 12000) * 3);
	t0 = GetTickCount();
	for( ;; ){
		EnterCriticalSection(&d->csAudio);
		if( d->tx.size() <= cap || !d->wantOut ){
			LeaveCriticalSection(&d->csAudio);
			return d->wantOut ? TRUE : FALSE;
		}
		if( (GetTickCount() - t0) > 2000 ){
			while( d->tx.size() > cap / 2 ){
				d->tx.pop_front();
				d->NoteSent(1);
			}
			LeaveCriticalSection(&d->csAudio);
			return TRUE;
		}
		LeaveCriticalSection(&d->csAudio);
		Sleep(5);
	}
}

int __fastcall CTciAudio::GetOutBC(void)
{
	Impl *d = m_impl;
	int n;
	EnterCriticalSection(&d->csAudio);
	n = d->block > 0 ? (int)d->tx.size() / d->block : 0;
	LeaveCriticalSection(&d->csAudio);
	return n;
}

int __fastcall CTciAudio::GetOutBCC(void)
{
	return m_impl->bcc;
}

void __fastcall CTciAudio::SetOutBCC(int count)
{
	m_impl->bcc = count;
	m_impl->bccFrac = 0;
}

int __fastcall CTciAudio::IsOutCritical(void)
{
	return GetOutBC() <= 2 ? 1 : 0;
}

int __fastcall CTciAudio::IsOutFull(void)
{
	Impl *d = m_impl;
	int full;
	EnterCriticalSection(&d->csAudio);
	full = (d->tx.size() >= (size_t)((d->tciRate > 1000 ? d->tciRate : 12000))) ? 1 : 0;
	LeaveCriticalSection(&d->csAudio);
	return full;
}

void __fastcall CTciAudio::SetPTT(int tx)
{
	Impl *d = m_impl;
	char buf[64];
	d->ptt = tx ? 1 : 0;
	if( !d->connected || !d->wantOut ) return;
	sprintf(buf, "TRX:%d,%s;", d->receiver, tx ? "true" : "false");
	d->SendText(buf);
}
