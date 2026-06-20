# BiTun Wrapper Component for ESP-IDF
# This compiles BiTun's source code from the assets/BiTun submodule folder.

idf_component_register(SRCS "../../assets/BiTun/src/ikcp.c"
                            "../../assets/BiTun/src/socks5.c"
                            "../../assets/BiTun/src/encrypt.c"
                            "../../assets/BiTun/src/tunnel.c"
                            "../../assets/BiTun/src/fec.c"
                            "bitun_osal_esp32.c"
                       INCLUDE_DIRS "../../assets/BiTun/src"
                       REQUIRES mbedtls lwip esp_timer esp_hw_support vfs)

target_compile_options(${COMPONENT_LIB} PRIVATE 
                       "-Wno-format" 
                       "-Wno-error=format" 
                       "-Wno-error=incompatible-pointer-types" 
                       "-Wno-incompatible-pointer-types")
