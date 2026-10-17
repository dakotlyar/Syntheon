# SPDX-License-Identifier: GPL-3.0-or-later
# The installed OpenSSL headers say the build has the hardened options of P17
# (no config autoload, no DSO / engine loading) and still has its assembly.
#   cmake -DCONFIG_H=<root>/include/openssl/configuration.h -P check-config.cmake
file(READ "${CONFIG_H}" text)
set(problems "")
foreach(def OPENSSL_NO_AUTOLOAD_CONFIG OPENSSL_NO_DSO OPENSSL_NO_ENGINE OPENSSL_NO_DYNAMIC_ENGINE OPENSSL_NO_APPS)
    if(NOT text MATCHES "define ${def}[\r\n]")
        list(APPEND problems "${def} is not defined")
    endif()
endforeach()
if(text MATCHES "define OPENSSL_NO_ASM[\r\n]")
    list(APPEND problems "OPENSSL_NO_ASM is defined: no hardware AES/SHA path")
endif()
if(problems)
    list(JOIN problems "\n  " problems)
    message(FATAL_ERROR "${CONFIG_H}:\n  ${problems}")
endif()
message(STATUS "OpenSSL configuration: hardened options present, assembly on")
