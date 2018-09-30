add_library(qni.connector.ws SHARED
    qni-connector-ws.c
    impls/handshake-util.c)

target_include_directories(qni.connector.ws PUBLIC
    ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(qni.connector.ws qni.core wslay nettle)

install(
    TARGETS qni.connector.ws
    LIBRARY DESTINATION lib)
