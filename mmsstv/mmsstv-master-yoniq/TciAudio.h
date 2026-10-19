//Copyright+LGPL
//-----------------------------------------------------------------------------------------------------------------------------------------------
// TCI audio device for MMSSTV / YONIQ.
// Audio is carried as TCI RX_AUDIO_STREAM / TX_AUDIO_STREAM over a WebSocket,
// so "TCI" can be chosen like a sound card.
//-----------------------------------------------------------------------------------------------------------------------------------------------
#ifndef TciAudioH
#define TciAudioH
//---------------------------------------------------------------------------
#define TCI_DEVICE_ID	(-3)

int __fastcall IsTciDeviceName(const char *name);

class CTciAudio
{
public:
	__fastcall CTciAudio();
	__fastcall ~CTciAudio();

	void __fastcall Configure(const char *host, int port, int receiver);
	void __fastcall Shutdown(void);

	BOOL __fastcall InOpen(int rate, int block);
	void __fastcall InClose(void);
	BOOL __fastcall InRead(double *p, int len);
	int __fastcall GetInBC(void);
	int __fastcall IsInCritical(void);

	BOOL __fastcall OutOpen(int rate, int block);
	void __fastcall OutAbort(void);
	BOOL __fastcall OutWrite(double *p, int len);
	int __fastcall GetOutBC(void);
	int __fastcall GetOutBCC(void);
	void __fastcall SetOutBCC(int count);
	int __fastcall IsOutCritical(void);
	int __fastcall IsOutFull(void);

	void __fastcall SetPTT(int tx);

private:
	struct Impl;
	Impl *m_impl;
};

extern CTciAudio TciAudio;
//---------------------------------------------------------------------------
#endif
