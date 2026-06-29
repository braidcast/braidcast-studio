# Kick OAuth (auth-code + PKCE over a loopback redirect) is a CONFIDENTIAL client:
# both a client_id AND a client_secret are injected at build time (each may be
# stored plaintext or nibble-XOR obfuscated under its own hex hash, 0 meaning
# plaintext). Gate on a non-empty id + secret with valid hex hashes. When unset,
# leave the feature off and blank every substitution so ui-config.h still
# compiles.
if(KICK_CLIENTID
   AND KICK_HASH MATCHES "^(0|[a-fA-F0-9]+)$"
   AND KICK_SECRET
   AND KICK_SECRET_HASH MATCHES "^(0|[a-fA-F0-9]+)$")
  target_enable_feature(${_target} "Kick API connection" KICK_ENABLED)
else()
  target_disable_feature(${_target} "Kick API connection")
  set(KICK_CLIENTID "")
  set(KICK_HASH "0")
  set(KICK_SECRET "")
  set(KICK_SECRET_HASH "0")
endif()
