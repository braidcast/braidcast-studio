#ifndef OBS_MULTISTREAM_FRONTEND_SOURCE_RENDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_SOURCE_RENDER_HPP_

#include <cstdint>

struct obs_source;
typedef struct obs_source obs_source_t;

struct obs_data;
typedef struct obs_data obs_data_t;

// Drawing ONE source into a display, aspect-letterboxed -- what the native windows
// bound to a single source (the interact window, the Filters dialog preview) render,
// as opposed to the preview surfaces, which render a whole canvas mix. Also the text
// labels the native windows draw over their own content.
namespace SourceRender {

// The source's base size: its intrinsic size when it has one, falling back to the
// global base resolution when it does not yet (a freshly added capture reports
// 0x0). Returns false when neither is available -- then the caller skips the frame.
bool BaseSize(obs_source_t *source, float &baseCX, float &baseCY);

// Render `source` -- its whole filter chain applied -- letterboxed into a display of
// (cx, cy) pixels. Runs on the libobs render thread, inside a draw callback.
void Letterboxed(obs_source_t *source, uint32_t cx, uint32_t cy);

// A private text_gdiplus label in bold Arial with an outline, holding `text`. `extra`
// (may be null) is applied over those settings for what differs per caller. Returns
// the creation ref, which the caller releases.
obs_source_t *CreateTextLabel(const char *name, const char *text, int fontSize, obs_data_t *extra);

} // namespace SourceRender

#endif // OBS_MULTISTREAM_FRONTEND_SOURCE_RENDER_HPP_
