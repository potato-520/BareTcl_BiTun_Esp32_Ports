# BiTun Wrapper Component for ESP-IDF
# This compiles BiTun's source code from the components/BiTun submodule folder.

idf_component_register(SRCS "../BiTun/src/ikcp.c"
                            "../BiTun/src/socks5.c"
                            "../BiTun/src/encrypt.c"
                            "../BiTun/src/tunnel.c"
                       INCLUDE_DIRS "../BiTun/src"
                       REQUIRES mbedtls lwip)
