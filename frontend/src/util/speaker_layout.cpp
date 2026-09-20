#include "speaker_layout.hpp"

namespace {

struct SpeakerName {
	speaker_layout layout;
	const char *name;
};

const SpeakerName kSpeakerNames[] = {
	{SPEAKERS_MONO, "mono"},   {SPEAKERS_STEREO, "stereo"}, {SPEAKERS_2POINT1, "2.1"}, {SPEAKERS_4POINT0, "4.0"},
	{SPEAKERS_4POINT1, "4.1"}, {SPEAKERS_5POINT1, "5.1"},   {SPEAKERS_7POINT1, "7.1"},
};

} // namespace

const char *Audio::SpeakerLayoutName(speaker_layout layout)
{
	for (const auto &s : kSpeakerNames) {
		if (s.layout == layout) {
			return s.name;
		}
	}
	return "stereo";
}

bool Audio::SpeakerLayoutFromName(const std::string &name, speaker_layout &out)
{
	for (const auto &s : kSpeakerNames) {
		if (name == s.name) {
			out = s.layout;
			return true;
		}
	}
	return false;
}
