# Kick OAuth now runs through the broker (no baked client_id/secret). Always enabled.
target_enable_feature(${_target} "Kick API connection" KICK_ENABLED)
