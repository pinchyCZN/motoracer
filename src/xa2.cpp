/*
 * xa2.cpp - XAudio2, behind the C surface in xa2.h.
 *
 * C++ for one reason and one only: xaudio2.h will not compile as C.
 * There is no C++ in the interface and none should appear in the
 * body beyond what COM forces - see xa2.h on why there is no policy
 * here.
 */

#include <windows.h>
#include <xaudio2.h>

#include "xa2.h"
#include "log.h"
#include "config.h"

static IXAudio2               *s_xa2;
static IXAudio2MasteringVoice *s_master;
static HMODULE                 s_dll;
static UINT32                  s_masterChannels;
static bool                    s_comInit;

/*
 * THE DLL IS LOADED BY HAND, NOT LINKED, AND THAT IS THE POINT.
 * Linking xaudio2.lib puts a STATIC import in the image, and a static
 * import that cannot be resolved stops Windows loading the process at
 * all - before main, before the log, leaving nothing to read
 * afterwards. The deliverable is an exe somebody drops into a stock
 * install (rule 10), so a machine without XAudio2 has to reach the
 * silent path rather than fail to start.
 *
 * 2.9 ships with Windows 10 and 2.8 with 8.x, so both are tried, then
 * 2.7 for Windows 7 - which is a different ABI, not just a third
 * name. See the block below.
 *
 * The header declares XAudio2Create __declspec(dllimport), which is
 * what forces a separate pointer type here: calling the declared name
 * is what would create the static import.
 */
typedef HRESULT (__stdcall *PFN_XAudio2Create)(IXAudio2 **, UINT32,
                                               XAUDIO2_PROCESSOR);

static const char *const kDlls[] = { "XAudio2_9.dll", "XAudio2_8.dll" };

/*
 * XAUDIO2 2.7, FOR WINDOWS 7, AND IT IS A DIFFERENT ABI - not just a
 * different DLL. Transcribed from the DirectX SDK (June 2010)
 * xaudio2.h, which is the only authoritative source: the Windows SDK
 * header #errors out below _WIN32_WINNT_WIN8 and says so.
 *
 * FOUR THINGS DIFFER, and every one of them is load-bearing:
 *
 * o 2.7 EXPORTS NO XAudio2Create. The DXSDK name is an inline helper
 *   that does CoCreateInstance then Initialize, so GetProcAddress
 *   finds nothing and the engine has to be built by hand.
 *
 * o IXAudio2 CARRIES THREE EXTRA SLOTS - GetDeviceCount,
 *   GetDeviceDetails and Initialize sit ahead of the common methods,
 *   so a 2.8 declaration driving a 2.7 object calls the wrong
 *   function pointer. That is why this interface is written out.
 *
 * o GetState TAKES ONE ARGUMENT, not two. Same slot, different
 *   __stdcall arity, so the 2.8 declaration would unbalance the
 *   stack. See XA27_SourceVoice.
 *
 * o XAUDIO2_VOICE_DETAILS GAINED ActiveFlags IN 2.8, between
 *   CreationFlags and InputChannels. Read a 2.7 object through the
 *   2.8 struct and the channel count comes back as the sample rate.
 *
 * IXAudio2Voice (19 slots) and IXAudio2SourceVoice (10) are
 * otherwise IDENTICAL in order, verified field by field against both
 * headers - which is what keeps every other call in this file
 * version-agnostic.
 *
 * The slots we never call still have to occupy their place; their
 * argument lists are not transcribed and must not be relied on.
 */
static const char *const kDll27 = "XAudio2_7.dll";

/* {5a508685-a254-4fba-9b82-9a24b00306af} and
   {8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb} - DXSDK xaudio2.h. */
static const GUID kClsidXAudio2_27 =
    { 0x5a508685, 0xa254, 0x4fba,
	  { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
static const GUID kIidIXAudio2_27 =
    { 0x8bcf1f58, 0x9fe7, 0x4583,
	  { 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb } };

/* 2.7's default is ANY_PROCESSOR; 2.8 redefined it as Processor1. */
#define XA27_ANY_PROCESSOR 0xffffffffu

struct XA27_DeviceDetails;      /* below - the interface only needs a ptr */

struct XA27_IXAudio2 {
	virtual HRESULT __stdcall QueryInterface(REFIID, void **) = 0;
	virtual ULONG   __stdcall AddRef(void) = 0;
	virtual ULONG   __stdcall Release(void) = 0;
	virtual HRESULT __stdcall GetDeviceCount(UINT32 *) = 0;
	virtual HRESULT __stdcall GetDeviceDetails(UINT32,
	                                           XA27_DeviceDetails *) = 0;
	virtual HRESULT __stdcall Initialize(UINT32, UINT32) = 0;
	virtual HRESULT __stdcall RegisterForCallbacks_unused(void) = 0;
	virtual void    __stdcall UnregisterForCallbacks_unused(void) = 0;
	virtual HRESULT __stdcall CreateSourceVoice(IXAudio2SourceVoice **,
	                                            const WAVEFORMATEX *,
	                                            UINT32, float, void *,
	                                            const void *,
	                                            const void *) = 0;
	virtual HRESULT __stdcall CreateSubmixVoice_unused(void) = 0;
	virtual HRESULT __stdcall CreateMasteringVoice(
	                              IXAudio2MasteringVoice **,
	                              UINT32, UINT32, UINT32, UINT32,
	                              const void *) = 0;
	virtual HRESULT __stdcall StartEngine(void) = 0;
	virtual void    __stdcall StopEngine(void) = 0;
	virtual HRESULT __stdcall CommitChanges_unused(void) = 0;
	virtual void    __stdcall GetPerformanceData_unused(void) = 0;
	virtual void    __stdcall SetDebugConfiguration_unused(void) = 0;
};

/*
 * ONLY GetState IS REACHED THROUGH THIS. It inherits IXAudio2Voice so
 * the compiler lays out those 19 slots rather than this file counting
 * them, and the six source methods ahead of GetState are placeholders
 * standing in for their slots.
 */
struct XA27_SourceVoice : public IXAudio2Voice {
	virtual HRESULT __stdcall Start_unused(void) = 0;
	virtual HRESULT __stdcall Stop_unused(void) = 0;
	virtual HRESULT __stdcall SubmitSourceBuffer_unused(void) = 0;
	virtual HRESULT __stdcall FlushSourceBuffers_unused(void) = 0;
	virtual HRESULT __stdcall Discontinuity_unused(void) = 0;
	virtual HRESULT __stdcall ExitLoop_unused(void) = 0;
	virtual void    __stdcall GetState(XAUDIO2_VOICE_STATE *) = 0;
};

/*
 * 2.7 PICKS THE OUTPUT BY INDEX AND 0 IS NOT "THE DEFAULT" - it is
 * whichever endpoint XAudio2 enumerated first. 2.8 replaced the
 * parameter with a device id where NULL does mean default, so the
 * all-defaults call this file makes means two different things. On a
 * box with more than one output that is silence with no error.
 */
struct XA27_DeviceDetails {
	WCHAR                DeviceID[256];
	WCHAR                DisplayName[256];
	UINT32               Role;
	WAVEFORMATEXTENSIBLE OutputFormat;
};

/* Any default role will do; DefaultGameDevice is preferred. */
#define XA27_ROLE_GAME  0x8u
#define XA27_ROLE_ANY   0xfu

/* 2.7's layout - no ActiveFlags. */
struct XA27_VoiceDetails {
	UINT32 CreationFlags;
	UINT32 InputChannels;
	UINT32 InputSampleRate;
};

static XA27_IXAudio2 *s_xa27;
static const char    *s_backend = "";
static UINT32         s_dev27;
static int            s_diag;

/* Defined beside xa2_close; xa2_open unwinds through it too. */
static void xa2_release_engine(void);

/*
 * THE FREQUENCY CEILING, AND WHY THE DEFAULT WILL NOT DO.
 *
 * XAudio2 fixes a voice's maximum playback ratio when the voice is
 * created and CLAMPS SILENTLY above it - no error, just a note that
 * stops rising. XAUDIO2_DEFAULT_FREQ_RATIO is 2.0, and the game
 * already comes within a whisker of it: the surface roll loop is
 * ROULEMAN.WAV at 11025 Hz driven to 20000 Hz, a ratio of 1.81.
 *
 * 4.0 is not a measurement, it is headroom - double the worst case
 * the shipped data reaches. The cost is a slightly larger resampler
 * buffer per voice.
 */
#define XA2_MAX_FREQ_RATIO 4.0f

/*
 * ORIGIN  NONE - ours
 *
 * Choose the output 2.7 will not choose for us. Logged either way:
 * if the wrong endpoint is still picked, the name is in the log.
 */
static void xa2_pick_device_27(void)
{
	XA27_DeviceDetails d;
	UINT32             count = 0;
	UINT32             i;
	int                best = -1;

	s_dev27 = 0;
	if (FAILED(s_xa27->GetDeviceCount(&count)) || count == 0) {
		mrlog_err("audio:  2.7 reports no output device\n");
		return;
	}

	for (i = 0; i < count; i++) {
		memset(&d, 0, sizeof d);
		if (FAILED(s_xa27->GetDeviceDetails(i, &d)))
			continue;
		mrlog("audio:  2.7 device %u role 0x%x %ls\n", i,
		      (unsigned)d.Role, d.DisplayName);
		if (best < 0 && (d.Role & XA27_ROLE_ANY) != 0)
			best = (int)i;
		if ((d.Role & XA27_ROLE_GAME) != 0)
			best = (int)i;
	}

	if (best >= 0)
		s_dev27 = (UINT32)best;
	mrlog("audio:  2.7 using device %u of %u\n", s_dev27, count);
}


/*
 * ORIGIN  NONE - ours
 *
 * The DXSDK XAudio2Create, by hand. CoCreateInstance then
 * Initialize is exactly what that inline helper does, and the COM
 * apartment xa2_open already entered is what makes it legal.
 */
static int xa2_open_27(void)
{
	HRESULT hr;

	s_dll = LoadLibraryA(kDll27);
	if (!s_dll)
		return 0;

	hr = CoCreateInstance(kClsidXAudio2_27, NULL, CLSCTX_INPROC_SERVER,
	                      kIidIXAudio2_27, (void **)&s_xa27);
	if (FAILED(hr) || !s_xa27) {
		s_xa27 = NULL;
		mrlog_err("audio:  XAudio2 2.7 CoCreateInstance failed"
		          " (0x%08lx)\n", (unsigned long)hr);
		FreeLibrary(s_dll);
		s_dll = NULL;
		return 0;
	}

	hr = s_xa27->Initialize(0, XA27_ANY_PROCESSOR);
	if (FAILED(hr)) {
		mrlog_err("audio:  XAudio2 2.7 Initialize failed (0x%08lx)\n",
		          (unsigned long)hr);
		s_xa27->Release();
		s_xa27 = NULL;
		FreeLibrary(s_dll);
		s_dll = NULL;
		return 0;
	}
	xa2_pick_device_27();

	/*
	 * StartEngine is documented as creating the processing thread.
	 * The engine starts of its own accord, so this is belt and
	 * braces rather than a fix - and it cannot hurt a running one.
	 */
	s_xa27->StartEngine();
	s_backend = kDll27;
	return 1;
}


/* ORIGIN  NONE - ours. */
extern "C" int xa2_open(void)
{
	HRESULT           hr;
	PFN_XAudio2Create create;
	int               i;
	int               force27 = 0;

	if (s_xa2 || s_xa27)
		return 1;

	/*
	 * A FAILURE HERE IS NOT FATAL AND MUST NOT UNBALANCE THE UNINIT.
	 * The shell may already have initialised COM on this thread under
	 * a different model, which returns RPC_E_CHANGED_MODE - harmless
	 * to us, but it means COM was not ours to close. Only a success
	 * arms the CoUninitialize in xa2_close.
	 */
	hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	s_comInit = (SUCCEEDED(hr) != 0);

	/*
	 * 2.9 then 2.8 then 2.7. The legacy path is last because it is
	 * the one with a redist behind it; a machine with an in-box
	 * runtime should never reach it.
	 *
	 * [DEBUG] forcexa27=1 SKIPS THE IN-BOX RUNTIME, which is the only
	 * way to exercise 2.7 off Windows 7. It needs XAudio2_7.dll, so
	 * the June 2010 redist.
	 */
	get_ini_value("DEBUG", "forcexa27", &force27);
	get_ini_value("DEBUG", "xa2diag", &s_diag);
	if (s_diag)
		s_diag = 6;
	if (force27)
		mrlog("audio:  [DEBUG] forcexa27 - 2.9/2.8 skipped\n");

	for (i = 0; !force27 && i < 2 && !s_dll; i++) {
		s_dll = LoadLibraryA(kDlls[i]);
		if (s_dll)
			s_backend = kDlls[i];
	}

	if (s_dll) {
		create = (PFN_XAudio2Create)GetProcAddress(s_dll,
		                                           "XAudio2Create");
		if (create) {
			hr = create(&s_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
			if (FAILED(hr))
				s_xa2 = NULL;
		}
		if (!s_xa2) {
			FreeLibrary(s_dll);
			s_dll = NULL;
		}
	}

	if (!s_xa2 && !xa2_open_27()) {
		mrlog_err("audio:  no usable XAudio2 runtime - silent\n");
		return 0;
	}

	/*
	 * ZEROS MEAN "MATCH THE DEVICE" - channels, rate, and the flags.
	 * There is no rate for the port to pick: the menu bank alone
	 * carries 5000, 11025, 16000 and 22050 Hz, and each source voice
	 * resamples from its own format, so a mastering voice pinned to
	 * one of them would be choosing a winner among four.
	 */
	hr = s_xa27 ? s_xa27->CreateMasteringVoice(&s_master, 0, 0, 0,
	                                           s_dev27, NULL)
	            : s_xa2->CreateMasteringVoice(&s_master);
	if (FAILED(hr) || !s_master) {
		s_master = NULL;
		mrlog_err("audio:  no mastering voice (0x%08lx) - silent\n",
		          (unsigned long)hr);
		xa2_release_engine();
		return 0;
	}

	/*
	 * TWO STRUCTS FOR ONE CALL, because 2.8 inserted ActiveFlags.
	 * GetVoiceDetails is the same slot and the same arity in both,
	 * so only the buffer it fills has to match the runtime.
	 */
	if (s_xa27) {
		XA27_VoiceDetails d;

		memset(&d, 0, sizeof d);
		s_master->GetVoiceDetails(
		    reinterpret_cast<XAUDIO2_VOICE_DETAILS *>(&d));
		s_masterChannels = d.InputChannels;
	} else {
		XAUDIO2_VOICE_DETAILS d;

		memset(&d, 0, sizeof d);
		s_master->GetVoiceDetails(&d);
		s_masterChannels = d.InputChannels;
	}
	mrlog("audio:  %s, mastering voice open, %u channels\n",
	      s_backend, s_masterChannels);
	return 1;
}

/* ORIGIN  NONE - ours. Whichever engine opened, released once. */
static void xa2_release_engine(void)
{
	if (s_xa2) {
		s_xa2->Release();
		s_xa2 = NULL;
	}
	if (s_xa27) {
		s_xa27->Release();
		s_xa27 = NULL;
	}
	if (s_dll) {
		FreeLibrary(s_dll);
		s_dll = NULL;
	}
}


extern "C" void xa2_close(void)
{
	/*
	 * ORDER: the mastering voice belongs to the engine, so it goes
	 * first, and the library is released only once both are gone.
	 * DestroyVoice returns nothing and cannot fail.
	 */
	if (s_master) {
		s_master->DestroyVoice();
		s_master = NULL;
	}
	xa2_release_engine();
	s_masterChannels = 0;
	if (s_comInit) {
		CoUninitialize();
		s_comInit = false;
	}
}

/*
 * The handle is the interface pointer. Nothing else is kept, because
 * everything else - which sample, which format, whether it may layer -
 * is audio.c's to decide.
 */
static IXAudio2SourceVoice *as_voice(XA2_Voice *v)
{
	return reinterpret_cast<IXAudio2SourceVoice *>(v);
}

extern "C" XA2_Voice *xa2_voice_create(int channels, int sampleRate,
                                       int bits)
{
	IXAudio2SourceVoice *sv = NULL;
	WAVEFORMATEX         fmt;
	HRESULT              hr;

	/*
	 * EITHER ENGINE WILL DO. Testing s_xa2 alone refused every voice
	 * on the 2.7 path and returned NULL without a word, so the game
	 * logged its cues and played nothing - no error anywhere.
	 */
	if ((!s_xa2 && !s_xa27) || channels <= 0 || sampleRate <= 0)
		return NULL;

	/*
	 * PCM ONLY, AND THE DERIVED FIELDS ARE DERIVED. Wave_Read forces
	 * wFormatTag to 1 and recomputes nBlockAlign and
	 * nAvgBytesPerSec rather than trusting the file, so the port
	 * does the same and wave.h carries only the three fields that
	 * really come off disk.
	 */
	memset(&fmt, 0, sizeof fmt);
	fmt.wFormatTag      = WAVE_FORMAT_PCM;
	fmt.nChannels       = (WORD)channels;
	fmt.nSamplesPerSec  = (DWORD)sampleRate;
	fmt.wBitsPerSample  = (WORD)bits;
	fmt.nBlockAlign     = (WORD)(channels * (bits / 8));
	fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

	hr = s_xa27 ? s_xa27->CreateSourceVoice(&sv, &fmt, 0,
	                                        XA2_MAX_FREQ_RATIO, NULL,
	                                        NULL, NULL)
	            : s_xa2->CreateSourceVoice(&sv, &fmt, 0,
	                                       XA2_MAX_FREQ_RATIO);
	if (FAILED(hr) || !sv) {
		mrlog_err("audio:  no source voice for %d Hz %d bit %dch "
		          "(0x%08lx)\n", sampleRate, bits, channels,
		          (unsigned long)hr);
		return NULL;
	}
	return reinterpret_cast<XA2_Voice *>(sv);
}

extern "C" void xa2_voice_destroy(XA2_Voice *v)
{
	if (!v)
		return;
	as_voice(v)->Stop(0, XAUDIO2_COMMIT_NOW);
	as_voice(v)->FlushSourceBuffers();
	as_voice(v)->DestroyVoice();
}

/*
 * ORIGIN  NONE - ours
 *
 * THE SECOND VOICE_DETAILS SITE. xa2_voice_pan read InputChannels
 * through the 2.8 struct, which on 2.7 lands on InputSampleRate -
 * so the mono test saw 22050 and the pan matrix was never applied.
 */
static UINT32 xa2_voice_channels(IXAudio2SourceVoice *sv)
{
	if (s_xa27) {
		XA27_VoiceDetails d;

		memset(&d, 0, sizeof d);
		sv->GetVoiceDetails(
		    reinterpret_cast<XAUDIO2_VOICE_DETAILS *>(&d));
		return d.InputChannels;
	}
	{
		XAUDIO2_VOICE_DETAILS d;

		memset(&d, 0, sizeof d);
		sv->GetVoiceDetails(&d);
		return d.InputChannels;
	}
}


/* One argument on 2.7 - see XA27_SourceVoice. */
static void xa2_voice_state(XA2_Voice *v, XAUDIO2_VOICE_STATE *st)
{
	if (s_xa27)
		reinterpret_cast<XA27_SourceVoice *>(v)->GetState(st);
	else
		as_voice(v)->GetState(st);
}


extern "C" int xa2_voice_busy(XA2_Voice *v)
{
	XAUDIO2_VOICE_STATE st;

	if (!v)
		return 0;
	memset(&st, 0, sizeof st);
	xa2_voice_state(v, &st);
	return st.BuffersQueued > 0;
}

extern "C" int xa2_voice_play(XA2_Voice *v, const void *pcm,
                              unsigned int bytes, int loop)
{
	IXAudio2SourceVoice *sv = as_voice(v);
	XAUDIO2_BUFFER       buf;
	HRESULT              hr;

	if (!v || !pcm || !bytes)
		return 0;

	sv->Stop(0, XAUDIO2_COMMIT_NOW);
	sv->FlushSourceBuffers();

	memset(&buf, 0, sizeof buf);
	buf.Flags      = XAUDIO2_END_OF_STREAM;
	buf.AudioBytes = bytes;
	buf.pAudioData = (const BYTE *)pcm;
	buf.LoopCount  = loop ? XAUDIO2_LOOP_INFINITE : 0;

	hr = sv->SubmitSourceBuffer(&buf);
	if (FAILED(hr)) {
		mrlog_err("audio:  submit failed (0x%08lx)\n",
		          (unsigned long)hr);
		return 0;
	}

	hr = sv->Start(0, XAUDIO2_COMMIT_NOW);

	/* [DEBUG] xa2diag=1 - the first few voices, in full. */
	if (s_diag > 0) {
		XAUDIO2_VOICE_STATE st;

		s_diag--;
		memset(&st, 0, sizeof st);
		xa2_voice_state(v, &st);
		mrlog("audio:  play %u B start=0x%08lx ch=%u queued=%u"
		      " played=%u\n", bytes, (unsigned long)hr,
		      xa2_voice_channels(sv), st.BuffersQueued,
		      (unsigned)st.SamplesPlayed);
	}
	return SUCCEEDED(hr);
}

extern "C" int xa2_voice_submit(XA2_Voice *v, const void *pcm,
                                unsigned int bytes)
{
	XAUDIO2_BUFFER buf;

	if (!v || !pcm || !bytes)
		return 0;

	/*
	 * NO FLAGS, and in particular no XAUDIO2_END_OF_STREAM: that
	 * marks the LAST buffer of a stream, and setting it on every
	 * block would tell XAudio2 the music ended two seconds in.
	 */
	memset(&buf, 0, sizeof buf);
	buf.AudioBytes = bytes;
	buf.pAudioData = (const BYTE *)pcm;

	return SUCCEEDED(as_voice(v)->SubmitSourceBuffer(&buf));
}

extern "C" int xa2_voice_start(XA2_Voice *v)
{
	if (!v)
		return 0;
	return SUCCEEDED(as_voice(v)->Start(0, XAUDIO2_COMMIT_NOW));
}

extern "C" int xa2_voice_queued(XA2_Voice *v)
{
	XAUDIO2_VOICE_STATE st;

	if (!v)
		return 0;
	memset(&st, 0, sizeof st);
	xa2_voice_state(v, &st);
	return (int)st.BuffersQueued;
}

extern "C" void xa2_voice_stop(XA2_Voice *v)
{
	if (!v)
		return;
	as_voice(v)->Stop(0, XAUDIO2_COMMIT_NOW);
	as_voice(v)->FlushSourceBuffers();
}

extern "C" void xa2_master_volume(float vol)
{
	if (s_master)
		s_master->SetVolume(vol);
}


void xa2_voice_volume(XA2_Voice *v, float vol)
{
	if (!v)
		return;
	if (vol < 0.0f)
		vol = 0.0f;
	as_voice(v)->SetVolume(vol, XAUDIO2_COMMIT_NOW);
}

extern "C" void xa2_voice_rate(XA2_Voice *v, float ratio)
{
	if (!v)
		return;
	if (ratio < XAUDIO2_MIN_FREQ_RATIO)
		ratio = XAUDIO2_MIN_FREQ_RATIO;
	if (ratio > XA2_MAX_FREQ_RATIO)
		ratio = XA2_MAX_FREQ_RATIO;
	as_voice(v)->SetFrequencyRatio(ratio, XAUDIO2_COMMIT_NOW);
}

extern "C" void xa2_voice_pan(XA2_Voice *v, float pan)
{
	IXAudio2SourceVoice  *sv = as_voice(v);
	float                 m[2];

	if (!v || s_masterChannels != 2)
		return;

	/*
	 * MONO SOURCES ONLY. A stereo sample already carries its own
	 * placement and the game ships none, so the matrix that would
	 * pan one is guesswork rather than transcription.
	 */
	if (xa2_voice_channels(sv) != 1)
		return;

	if (pan < -1.0f)
		pan = -1.0f;
	if (pan > 1.0f)
		pan = 1.0f;

	/*
	 * ATTENUATE THE FAR SIDE, DO NOT SPLIT THE NEAR ONE. Centre is
	 * 1.0 into both, which is what DirectSound's pan does - a matrix
	 * of 0.5/0.5 would drop every centred sound by 6 dB, and since
	 * the loaders pan everything to 0 that would be the whole game.
	 *
	 * DIVERGENCE: DirectSound attenuates in hundredths of a decibel
	 * and this is linear in amplitude. The curve differs; the
	 * endpoints and the centre do not.
	 */
	m[0] = (pan > 0.0f) ? (1.0f - pan) : 1.0f;
	m[1] = (pan < 0.0f) ? (1.0f + pan) : 1.0f;
	sv->SetOutputMatrix(NULL, 1, 2, m, XAUDIO2_COMMIT_NOW);
}

extern "C" void xa2_voice_pause(XA2_Voice *v)
{
	if (!v)
		return;
	/* NO FlushSourceBuffers - see xa2.h. The buffer and the play
	 * cursor stay put so Start resumes where it left off. */
	as_voice(v)->Stop(0, XAUDIO2_COMMIT_NOW);
}

extern "C" void xa2_voice_resume(XA2_Voice *v)
{
	if (!v)
		return;
	as_voice(v)->Start(0, XAUDIO2_COMMIT_NOW);
}
