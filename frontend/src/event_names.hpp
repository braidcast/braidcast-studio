#ifndef OBS_MULTISTREAM_FRONTEND_EVENT_NAMES_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENT_NAMES_HPP_

// Bridge PUSH-EVENT names (C++ -> JS via Bridge::EmitEvent).
// Mirror of web/src/lib/eventNames.ts -- values MUST match exactly.

namespace EventNames {
inline constexpr const char *kStreamProfileChanged = "streamProfile.changed";
inline constexpr const char *kOutputBindingChanged = "outputBinding.changed";
inline constexpr const char *kCanvasChanged = "canvas.changed";
inline constexpr const char *kCollectionsChanged = "collections.changed";
inline constexpr const char *kStreamingChanged = "streaming.changed";
inline constexpr const char *kProjectorChanged = "projector.changed";
inline constexpr const char *kOverlaysChanged = "overlays.changed";
inline constexpr const char *kTransitionsChanged = "transitions.changed";
inline constexpr const char *kScenesChanged = "scenes.changed";
inline constexpr const char *kSceneLinkChanged = "sceneLink.changed";
inline constexpr const char *kMcpChanged = "mcp.changed";
inline constexpr const char *kHotkeysChanged = "hotkeys.changed";
inline constexpr const char *kAudioChanged = "audio.changed";
inline constexpr const char *kWindowStateChanged = "window.stateChanged";
inline constexpr const char *kWindowClosed = "window.closed";
inline constexpr const char *kWindowOpened = "window.opened";
inline constexpr const char *kSettingsVideoChanged = "settings.videoChanged";
inline constexpr const char *kSettingsAudioChanged = "settings.audioChanged";
inline constexpr const char *kSettingsGeneralChanged = "settings.generalChanged";
inline constexpr const char *kSettingsAdvancedChanged = "settings.advancedChanged";
inline constexpr const char *kScreenshotSaved = "screenshot.saved";
inline constexpr const char *kOauthStatus = "oauth.status";
inline constexpr const char *kOauthConnectProgress = "oauth.connectProgress";
inline constexpr const char *kOauthConnectError = "oauth.connectError";
inline constexpr const char *kMultistreamChanged = "multistream.changed";
inline constexpr const char *kEventsNew = "events.new";
inline constexpr const char *kEventsBackfill = "events.backfill";
inline constexpr const char *kVirtualCamChanged = "virtualCam.changed";
inline constexpr const char *kUndoChanged = "undo.changed";
inline constexpr const char *kStreamMetaChanged = "streamMeta.changed";
inline constexpr const char *kSceneItemsChanged = "sceneItems.changed";
inline constexpr const char *kSceneItemSelected = "sceneItem.selected";
inline constexpr const char *kPreviewContextMenu = "preview.contextMenu";
inline constexpr const char *kInteractChanged = "interact.changed";
inline constexpr const char *kChatState = "chat.state";
inline constexpr const char *kChatMessage = "chat.message";
inline constexpr const char *kChannelsStats = "channels.stats";
inline constexpr const char *kViewersChanged = "viewers.changed";
inline constexpr const char *kAudioLevels = "audio.levels";
inline constexpr const char *kObsEvent = "obs.event";
} // namespace EventNames

#endif // OBS_MULTISTREAM_FRONTEND_EVENT_NAMES_HPP_
