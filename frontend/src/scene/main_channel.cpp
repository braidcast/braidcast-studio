#include "main_channel.hpp"

#include "multistream/VideoGate.hpp"

namespace MainChannel {

void Set(obs_source_t *source)
{
	obs_set_output_source(0, source);
	VideoGate::Reconcile(); // the incoming occupant's tree is a different gated set
}

} // namespace MainChannel
