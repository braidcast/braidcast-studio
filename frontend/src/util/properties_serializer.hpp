#ifndef OBS_MULTISTREAM_FRONTEND_PROPERTIES_SERIALIZER_HPP_
#define OBS_MULTISTREAM_FRONTEND_PROPERTIES_SERIALIZER_HPP_

#include <obs.h>

#include <nlohmann/json.hpp>

// Generic obs_properties -> JSON serializer. Given an obs_properties_t and the
// current obs_data settings, produce a JSON array of property descriptors the
// Svelte dynamic form renders. Type dispatch is a switch over obs_property_type
// (the canonical dispatch); each descriptor carries the type-specific fields the
// UI needs. Groups recurse into nested `props`.
//
// This is frontend-agnostic and reusable across kinds (source now; encoder /
// service / output later just supply their own obs_properties + settings).
namespace PropertiesSerializer {

using json = nlohmann::json;

// Serialize every property in `props`, reading current values from `settings`.
// `settings` may be null (treated as empty). Returns a JSON array of descriptors
// in declaration order. Does not take ownership of either argument.
json SerializeProperties(obs_properties_t *props, obs_data_t *settings);

} // namespace PropertiesSerializer

#endif // OBS_MULTISTREAM_FRONTEND_PROPERTIES_SERIALIZER_HPP_
