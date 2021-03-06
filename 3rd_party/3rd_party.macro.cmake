﻿# =========== 3rd_party =========== 
if (NOT PROJECT_3RD_PARTY_PACKAGE_DIR)
    set (PROJECT_3RD_PARTY_PACKAGE_DIR "${CMAKE_CURRENT_LIST_DIR}/packages")
endif ()
if (NOT PROJECT_3RD_PARTY_INSTALL_DIR)
    set (PROJECT_3RD_PARTY_INSTALL_DIR "${CMAKE_CURRENT_LIST_DIR}/install/${PROJECT_PREBUILT_PLATFORM_NAME}")
endif ()

if (NOT EXISTS ${PROJECT_3RD_PARTY_PACKAGE_DIR})
    file(MAKE_DIRECTORY ${PROJECT_3RD_PARTY_PACKAGE_DIR})
endif ()

if (NOT EXISTS ${PROJECT_3RD_PARTY_INSTALL_DIR})
    file(MAKE_DIRECTORY ${PROJECT_3RD_PARTY_INSTALL_DIR})
endif ()

set (PROJECT_3RD_PARTY_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})
include("${PROJECT_3RD_PARTY_ROOT_DIR}/fmtlib/fmtlib.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/libuv/libuv.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/protobuf/protobuf.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/atframe_utils/libatframe_utils.cmake")