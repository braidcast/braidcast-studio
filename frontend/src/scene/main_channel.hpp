#ifndef OBS_MULTISTREAM_FRONTEND_MAIN_CHANNEL_HPP_
#define OBS_MULTISTREAM_FRONTEND_MAIN_CHANNEL_HPP_

#include <obs.h>

// The sole writer of global output channel 0 (the Main/Default program chain).
// Routing every bind through here is what lets VideoGate re-derive its gated set
// the instant channel 0's occupant changes: the set is a function of the
// occupant's scene tree, and nothing else signals that the tree was swapped.
namespace MainChannel {

// Bind `source` (may be null) to output channel 0.
void Set(obs_source_t *source);

} // namespace MainChannel

#endif // OBS_MULTISTREAM_FRONTEND_MAIN_CHANNEL_HPP_
