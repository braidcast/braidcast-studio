# Twitch signs in through the broker like every other provider, which holds the
# client secret; the app keeps only Twitch's public client_id, which Helix wants
# as the Client-Id header on every request. So only the id is injected at build
# time, never a secret. Gate purely on a non-empty client_id plus a valid hex obfuscation key
# (0 means the client_id is stored as plaintext). When unset, leave the feature
# off and blank the substitutions so ui-config.h still compiles.
if(TWITCH_CLIENTID AND TWITCH_HASH MATCHES "^(0|[a-fA-F0-9]+)$")
  target_enable_feature(${_target} "Twitch API connection" TWITCH_ENABLED)
else()
  target_disable_feature(${_target} "Twitch API connection")
  set(TWITCH_CLIENTID "")
  set(TWITCH_HASH "0")
endif()
