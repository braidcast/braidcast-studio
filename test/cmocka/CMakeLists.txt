project(obs-cmocka)

find_package(CMocka CONFIG REQUIRED)

# Serializer test
add_executable(test_serializer test_serializer.c)
target_include_directories(test_serializer PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_serializer PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})
add_test(test_serializer ${CMAKE_CURRENT_BINARY_DIR}/test_serializer)

# darray test
add_executable(test_darray test_darray.c)
target_include_directories(test_darray PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_darray PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})

add_test(test_darray ${CMAKE_CURRENT_BINARY_DIR}/test_darray)

# bitstream test
add_executable(test_bitstream test_bitstream.c)
target_include_directories(test_bitstream PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_bitstream PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})

add_test(test_bitstream ${CMAKE_CURRENT_BINARY_DIR}/test_bitstream)

# OS path test
add_executable(test_os_path test_os_path.c)
target_include_directories(test_os_path PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_os_path PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})

add_test(test_os_path ${CMAKE_CURRENT_BINARY_DIR}/test_os_path)

# Audio-group voice pick test. The pick is internal to libobs, so its source is compiled in.
add_executable(test_audio_voice test_audio_voice.c ${CMAKE_SOURCE_DIR}/libobs/obs-audio-voice.c)
target_include_directories(test_audio_voice PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs)
target_link_libraries(test_audio_voice PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})

add_test(test_audio_voice ${CMAKE_CURRENT_BINARY_DIR}/test_audio_voice)
