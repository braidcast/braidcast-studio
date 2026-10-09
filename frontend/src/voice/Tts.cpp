#include "voice/Tts.hpp"

#include "util/text_encoding.hpp"
#include "voice/VoiceResampler.hpp"

#include <windows.h>
#include <objbase.h>
#include <sapi.h>

#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace Voice {

namespace {

using Microsoft::WRL::ComPtr;

// The three SAPI GUIDs this file needs, defined here rather than taken from sapi.lib.
// sapi.h declares them extern; the CLSIDs come from the SDK's sapi.lib, but
// SPDFID_WaveFormatEx is not in every import library (MinGW's libsapi.a defines the
// CLSIDs and IIDs and not it; Wine defines its own copy for the same reason), so a
// local copy keeps the link independent of which SDK builds this. Values from sapi.h
// (CLSID_SpVoice, CLSID_SpStream) and sapi.idl (SPDFID_WaveFormatEx).
constexpr GUID kClsidSpVoice = {0x96749377, 0x3391, 0x11d2, {0x9e, 0xe3, 0x00, 0xc0, 0x4f, 0x79, 0x73, 0x96}};
constexpr GUID kClsidSpStream = {0x715d9c59, 0x4442, 0x11d2, {0x96, 0x05, 0x00, 0xc0, 0x4f, 0x8e, 0xe6, 0x28}};
constexpr GUID kFormatWaveFormatEx = {0xc31adbae, 0x527f, 0x4ff5, {0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c}};

// 16 kHz mono 16-bit: what the recognizer already speaks, and a format every SAPI
// voice converts to.
WAVEFORMATEX MonoFormat()
{
	WAVEFORMATEX format = {};
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 1;
	format.nSamplesPerSec = kVoiceSampleRate;
	format.wBitsPerSample = 16;
	format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
	format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
	return format;
}

// 16-bit PCM bytes, interleaved, into mono floats. The stream should hold raw PCM; if a
// RIFF header ever precedes it, the samples start after its "data" chunk header.
bool DecodePcm16(const std::vector<uint8_t> &bytes, unsigned channels, unsigned rate, WavData &out)
{
	size_t offset = 0;
	if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
	    std::memcmp(bytes.data() + 8, "WAVE", 4) == 0) {
		for (size_t at = 12; at + 8 <= bytes.size(); ++at) {
			if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
				offset = at + 8;
				break;
			}
		}
	}
	if (channels == 0 || rate == 0) {
		return false;
	}
	const size_t frame = static_cast<size_t>(channels) * sizeof(int16_t);
	const size_t frames = (bytes.size() - offset) / frame;
	out.sampleRate = static_cast<int>(rate);
	out.samples.assign(frames, 0.f);
	for (size_t i = 0; i < frames; ++i) {
		float sum = 0.f;
		for (unsigned c = 0; c < channels; ++c) {
			int16_t s = 0;
			std::memcpy(&s, bytes.data() + offset + i * frame + c * sizeof(int16_t), sizeof s);
			sum += static_cast<float>(s) / 32768.0f;
		}
		out.samples[i] = sum / static_cast<float>(channels);
	}
	return !out.samples.empty();
}

// The COM half. Every interface is released before it returns, so the caller can
// uninitialize COM after it.
bool SpeakToMemory(const std::wstring &wide, WavData &out, std::string &error)
{
	ComPtr<ISpVoice> voice;
	HRESULT hr = CoCreateInstance(kClsidSpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(voice.GetAddressOf()));
	if (FAILED(hr)) {
		error = "no speech synthesizer is available on this system";
		return false;
	}

	// Synthesize into memory rather than to the speakers: the result goes through the
	// monitor-only cue source, so it reaches the user and not the stream.
	ComPtr<IStream> memory;
	hr = CreateStreamOnHGlobal(nullptr, TRUE, memory.GetAddressOf());
	if (FAILED(hr)) {
		error = "could not allocate the speech buffer";
		return false;
	}
	ComPtr<ISpStream> stream;
	hr = CoCreateInstance(kClsidSpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(stream.GetAddressOf()));
	if (FAILED(hr)) {
		error = "could not create the speech stream";
		return false;
	}
	const WAVEFORMATEX format = MonoFormat();
	hr = stream->SetBaseStream(memory.Get(), kFormatWaveFormatEx, &format);
	if (FAILED(hr)) {
		error = "the speech synthesizer refused the audio format";
		return false;
	}
	if (FAILED(voice->SetOutput(stream.Get(), TRUE))) {
		error = "could not route the synthesizer to memory";
		return false;
	}

	// SPF_IS_NOT_XML: the text is the user's own scene and source names, which may well
	// contain angle brackets. Parsing it as markup would drop them or fail. Synchronous:
	// Speak returns once the whole text is in the stream.
	hr = voice->Speak(wide.c_str(), SPF_DEFAULT | SPF_IS_NOT_XML, nullptr);
	if (FAILED(hr)) {
		error = "the speech synthesizer failed";
		return false;
	}

	// The format the stream actually holds, rather than the one asked for.
	unsigned channels = format.nChannels;
	unsigned rate = format.nSamplesPerSec;
	GUID formatId = {};
	WAVEFORMATEX *actual = nullptr;
	if (SUCCEEDED(stream->GetFormat(&formatId, &actual)) && actual) {
		const bool pcm16 = actual->wFormatTag == WAVE_FORMAT_PCM && actual->wBitsPerSample == 16;
		channels = actual->nChannels;
		rate = actual->nSamplesPerSec;
		CoTaskMemFree(actual);
		if (!pcm16) {
			error = "the speech synthesizer produced an unexpected audio format";
			return false;
		}
	}
	voice.Reset(); // lets go of the stream, so everything it wrote is in memory

	// Read the PCM back out of the memory stream.
	STATSTG stat = {};
	if (FAILED(memory->Stat(&stat, STATFLAG_NONAME))) {
		error = "could not measure the synthesized audio";
		return false;
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
	LARGE_INTEGER zero = {};
	ULONG read = 0;
	if (bytes.empty() || FAILED(memory->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
	    FAILED(memory->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read))) {
		error = "the synthesizer produced no audio";
		return false;
	}
	bytes.resize(read);
	if (!DecodePcm16(bytes, channels, rate, out)) {
		error = "the synthesizer produced no audio";
		return false;
	}
	return true;
}

} // namespace

bool Synthesize(const std::string &text, WavData &out, std::string &error)
{
	if (text.empty()) {
		error = "nothing to say";
		return false;
	}
	if (text.size() > kMaxSpokenChars) {
		error = "that is too long to read back";
		return false;
	}
	const std::wstring wide = Encoding::Utf8ToWide(text);
	if (wide.empty()) {
		error = "that text cannot be spoken";
		return false;
	}

	// A worker thread of our own, so COM is ours to set up here. An apartment someone
	// already chose (RPC_E_CHANGED_MODE) still works for SAPI; only a successful init
	// is balanced.
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool uninit = SUCCEEDED(coInit);
	WavData spoken;
	const bool ok = SpeakToMemory(wide, spoken, error);
	if (uninit) {
		CoUninitialize();
	}
	if (ok) {
		out = std::move(spoken);
	}
	return ok;
}

} // namespace Voice
