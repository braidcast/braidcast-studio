# YouTube OAuth now runs through the broker (no baked client_id/secret). Always enabled.
target_enable_feature(${_target} "YouTube API connection" YOUTUBE_ENABLED)
