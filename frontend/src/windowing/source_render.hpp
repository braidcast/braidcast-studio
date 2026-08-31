#ifndef OBS_MULTISTREAM_FRONTEND_SOURCE_RENDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_SOURCE_RENDER_HPP_

#include <cstdint>

struct obs_source;
typedef struct obs_source obs_source_t;

// Drawing ONE source into a display, aspect-letterboxed -- what the native windows
// bound to a single source (the interact window, the Filters dialog preview) render,
// as opposed to the preview surfaces, which render a whole canvas mix.
namespace SourceRender {

// The source's base size: its intrinsic size when it has one, falling back to the
// global base resolution when it does not yet (a freshly added capture reports
// 0x0). Returns false when neither is available -- then the caller skips the frame.
bool BaseSize(obs_source_t *source, float &baseCX, float &baseCY);

// Render `source` -- its whole filter chain applied -- letterboxed into a display of
// (cx, cy) pixels. Runs on the libobs render thread, inside a draw callback.
void Letterboxed(obs_source_t *source, uint32_t cx, uint32_t cy);

} // namespace SourceRender

#endif // OBS_MULTISTREAM_FRONTEND_SOURCE_RENDER_HPP_
