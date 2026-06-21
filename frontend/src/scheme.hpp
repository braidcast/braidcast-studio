#ifndef OBS_MULTISTREAM_FRONTEND_SCHEME_HPP_
#define OBS_MULTISTREAM_FRONTEND_SCHEME_HPP_

// The custom scheme name. The UI's origin is app://app/ — the first "app" is the
// scheme, the second is the (single) host. index.html lives at app://app/index.html.
inline constexpr char kAppScheme[] = "app";
inline constexpr char kAppHost[] = "app";

// Register the scheme handler factory that serves the offline bundle from the
// rundir's data/obs-multistream/web/ directory. Browser process, after init.
void RegisterAppSchemeHandlerFactory();

#endif // OBS_MULTISTREAM_FRONTEND_SCHEME_HPP_
