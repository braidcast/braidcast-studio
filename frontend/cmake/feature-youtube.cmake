# YouTube OAuth (auth-code + PKCE over a loopback redirect) reuses the Kick
# strategy but the secret is OPTIONAL: Google desktop clients ship a
# non-confidential secret that the token calls send only when present. Gate purely
# on a non-empty client_id plus valid hex obfuscation keys (0 means plaintext);
# the secret may be empty. When unset, leave the feature off and blank every
# substitution so ui-config.h still compiles.
if(YOUTUBE_CLIENTID
   AND YOUTUBE_HASH MATCHES "^(0|[a-fA-F0-9]+)$"
   AND YOUTUBE_SECRET_HASH MATCHES "^(0|[a-fA-F0-9]+)$")
  target_enable_feature(${_target} "YouTube API connection" YOUTUBE_ENABLED)
else()
  target_disable_feature(${_target} "YouTube API connection")
  set(YOUTUBE_CLIENTID "")
  set(YOUTUBE_HASH "0")
  set(YOUTUBE_SECRET "")
  set(YOUTUBE_SECRET_HASH "0")
endif()
