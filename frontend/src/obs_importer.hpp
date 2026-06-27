#ifndef OBS_MULTISTREAM_FRONTEND_OBS_IMPORTER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OBS_IMPORTER_HPP_

#include <nlohmann/json.hpp>

#include <string>

// Read-only importer of an EXTERNAL OBS Studio install's data into THIS fork.
//
// The fork stores its own data under <config>/obs-multistream and never shares
// files with a real OBS Studio install (<config>/obs-studio). This importer reads
// the obs-studio tree STRICTLY READ-ONLY -- scene collections (basic/scenes/*.json),
// the active profile's stream service (basic/profiles/<dir>/service.json) and its
// video/audio config (basic/profiles/<dir>/basic.ini) -- and produces NEW fork
// collections / stream profiles / canvas + audio state. It NEVER opens an
// obs-studio path for writing: JSON is read with obs_data_create_from_json_file and
// the ini with config_open(..., CONFIG_OPEN_EXISTING), both read-only; the only
// files written are under the fork's own obs-multistream/basic dir.
namespace ObsImporter {

// Inventory an obs-studio config dir without mutating anything. `path` empty ->
// os_get_config_path("obs-studio"). Shape (see bridge importer.scan):
//   { found, path, collections:[{name,file,scenes:[string]}],
//     service:{present,label}|null, video:{...}|null, audio:{...}|null }
// Not found -> { found:false, path:"", collections:[] }. A malformed collection
// file is skipped (it just won't appear), never fatal.
nlohmann::json Scan(const std::string &path);

// Perform the import described by `params` (see bridge importer.import). Refuses
// (before any mutation) while any output is live. Creates fork collections /
// profiles / canvas + audio state and emits the matching change events. Returns
//   { ok:true, imported:{collections:N,service,video,audio}, warnings:[string] }
// or { ok:false, error }.
nlohmann::json Import(const nlohmann::json &params);

} // namespace ObsImporter

#endif // OBS_MULTISTREAM_FRONTEND_OBS_IMPORTER_HPP_
