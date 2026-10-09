#include "voice/WavFile.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace Voice {

namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xfffe;
constexpr size_t kMaxFileBytes = 32u << 20; // the assets are tens of kilobytes

uint16_t ReadU16(const unsigned char *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ReadU32(const unsigned char *p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
	       (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

bool LoadWavMono(const std::string &utf8Path, WavData &out, std::string &error)
{
	std::error_code ec;
	const auto size = std::filesystem::file_size(std::filesystem::u8path(utf8Path), ec);
	if (ec) {
		error = "cannot open " + utf8Path;
		return false;
	}
	if (size < 44 || size > kMaxFileBytes) {
		error = "not a usable WAV file (" + std::to_string(size) + " bytes)";
		return false;
	}
	std::ifstream in(std::filesystem::u8path(utf8Path), std::ios::binary);
	std::vector<unsigned char> bytes(static_cast<size_t>(size));
	in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!in) {
		error = "could not read " + utf8Path;
		return false;
	}
	if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
		error = "not a RIFF/WAVE file";
		return false;
	}

	uint16_t format = 0;
	uint16_t channels = 0;
	uint16_t bits = 0;
	uint32_t rate = 0;
	const unsigned char *data = nullptr;
	size_t dataBytes = 0;

	// Walk the chunk list rather than assuming "fmt " then "data": the synthesizer
	// writes a LIST/INFO chunk between them.
	size_t pos = 12;
	while (pos + 8 <= bytes.size()) {
		const unsigned char *id = bytes.data() + pos;
		const uint32_t chunkSize = ReadU32(bytes.data() + pos + 4);
		const size_t body = pos + 8;
		if (body + chunkSize > bytes.size()) {
			break;
		}
		if (std::memcmp(id, "fmt ", 4) == 0 && chunkSize >= 16) {
			format = ReadU16(bytes.data() + body);
			channels = ReadU16(bytes.data() + body + 2);
			rate = ReadU32(bytes.data() + body + 4);
			bits = ReadU16(bytes.data() + body + 14);
			if (format == kFormatExtensible && chunkSize >= 40) {
				// The real format is the first two bytes of the GUID sub-format.
				format = ReadU16(bytes.data() + body + 24);
			}
		} else if (std::memcmp(id, "data", 4) == 0) {
			data = bytes.data() + body;
			dataBytes = chunkSize;
		}
		pos = body + chunkSize + (chunkSize & 1); // chunks are word aligned
	}

	if (!data || channels == 0 || rate == 0) {
		error = "WAV file has no usable fmt/data chunk";
		return false;
	}
	const bool pcm16 = format == kFormatPcm && bits == 16;
	const bool float32 = format == kFormatFloat && bits == 32;
	if (!pcm16 && !float32) {
		error = "unsupported WAV format (" + std::to_string(format) + ", " + std::to_string(bits) + " bit)";
		return false;
	}

	const size_t bytesPerSample = bits / 8;
	const size_t frames = dataBytes / (bytesPerSample * channels);
	out.sampleRate = static_cast<int>(rate);
	out.samples.resize(frames);
	for (size_t f = 0; f < frames; ++f) {
		double sum = 0.0;
		for (size_t c = 0; c < channels; ++c) {
			const unsigned char *p = data + (f * channels + c) * bytesPerSample;
			if (pcm16) {
				sum += static_cast<double>(static_cast<int16_t>(ReadU16(p))) / 32768.0;
			} else {
				float v = 0.f;
				std::memcpy(&v, p, sizeof(v));
				sum += v;
			}
		}
		out.samples[f] = static_cast<float>(sum / static_cast<double>(channels));
	}
	return true;
}

} // namespace Voice
