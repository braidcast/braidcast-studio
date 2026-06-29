# Twitch OAuth (device-code flow) is a public client: only a client_id is
# injected at build time, never a secret, and the flow needs no CEF browser
# panels. Gate purely on a non-empty client_id plus a valid hex obfuscation key
# (0 means the client_id is stored as plaintext). When unset, leave the feature
# off and blank the substitutions so ui-config.h still compiles.
if(TWITCH_CLIENTID AND TWITCH_HASH MATCHES "^(0|[a-fA-F0-9]+)$")
  target_enable_feature(${_target} "Twitch API connection" TWITCH_ENABLED)
else()
  target_disable_feature(${_target} "Twitch API connection")
  set(TWITCH_CLIENTID "")
  set(TWITCH_HASH "0")
endif()
