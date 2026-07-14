cmake_minimum_required(VERSION 3.16)

project(seafile VERSION 6.0.1 LANGUAGES C)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB 1.2 REQUIRED)

option(SEAFILE_BUILD_SERVER "Build seaf-server, seafserv-gc and seaf-fsck" ON)
option(SEAFILE_BUILD_CONTROLLER "Build seafile-controller" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FUSE_DEFAULT ON)
else()
    set(FUSE_DEFAULT OFF)
endif()
option(SEAFILE_BUILD_FUSE "Build seaf-fuse" ${FUSE_DEFAULT})
option(SEAFILE_ENABLE_HTTPSERVER "Enable the legacy evhtp HTTP server" ON)
option(SEAFILE_INSTALL_PYTHON "Install Python RPC modules" ON)

pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET "glib-2.0>=2.16.0")
pkg_check_modules(GOBJECT REQUIRED IMPORTED_TARGET "gobject-2.0>=2.16.0")
pkg_check_modules(SEARPC REQUIRED "libsearpc>=1.0")
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET "jansson>=2.2.1")
pkg_check_modules(LIBEVENT REQUIRED IMPORTED_TARGET "libevent>=2.0")
pkg_check_modules(UUID REQUIRED IMPORTED_TARGET uuid)

find_program(VALAC valac REQUIRED)
find_program(SEARPC_CODEGEN searpc-codegen.py REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GENERATE OUTPUT "${GENERATED_DIR}/config.h" CONTENT "#pragma once\n")
set(VALA_NAMES repo commit dirent dir filelock task branch crypt webaccess seahub copy-task ccnetobj search-result)
list(TRANSFORM VALA_NAMES APPEND ".vala")
list(TRANSFORM VALA_NAMES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/lib/" OUTPUT_VARIABLE VALA_SOURCES)
list(TRANSFORM VALA_SOURCES REPLACE "\\.vala$" ".c" OUTPUT_VARIABLE VALA_C_SOURCES)
list(TRANSFORM VALA_C_SOURCES REPLACE "^${CMAKE_CURRENT_SOURCE_DIR}/lib/" "${GENERATED_DIR}/")

add_custom_command(
    OUTPUT ${VALA_C_SOURCES} "${GENERATED_DIR}/seafile-object.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${GENERATED_DIR}"
            "${VALAC}" -C --pkg posix -H seafile-object.h ${VALA_SOURCES}
    DEPENDS ${VALA_SOURCES}
    VERBATIM)

set(SEARPC_HEADERS
    "${GENERATED_DIR}/searpc-signature.h"
    "${GENERATED_DIR}/searpc-marshal.h")
add_custom_command(
    OUTPUT ${SEARPC_HEADERS}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${GENERATED_DIR}"
            "${Python3_EXECUTABLE}" "${SEARPC_CODEGEN}" "${CMAKE_CURRENT_SOURCE_DIR}/lib/rpc_table.py"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lib/rpc_table.py"
    VERBATIM)

# libsearpc 3.1 installs headers directly in includedir but advertises an
# additional, sometimes absent, includedir/searpc path in its pkg-config file.
add_library(searpc_deps INTERFACE)
target_include_directories(searpc_deps INTERFACE "${SEARPC_INCLUDEDIR}")
target_link_directories(searpc_deps INTERFACE ${SEARPC_LIBRARY_DIRS})
target_link_libraries(searpc_deps INTERFACE ${SEARPC_LIBRARIES})
target_compile_options(searpc_deps INTERFACE ${SEARPC_CFLAGS_OTHER})

add_library(seafile_common STATIC
    ${VALA_C_SOURCES}
    lib/net.c
    lib/bloom-filter.c
    lib/utils.c
    lib/db.c
    lib/job-mgr.c
    lib/timer.c)
target_include_directories(seafile_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${GENERATED_DIR}")
target_link_libraries(seafile_common PUBLIC
    PkgConfig::GLIB PkgConfig::GOBJECT searpc_deps PkgConfig::LIBEVENT
    OpenSSL::Crypto SQLite::SQLite3 Threads::Threads ZLIB::ZLIB PkgConfig::UUID)
target_compile_options(seafile_common PRIVATE -Wall)

add_library(cdc STATIC common/cdc/cdc.c common/cdc/rabin-checksum.c)
target_include_directories(cdc PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib")
target_link_libraries(cdc PUBLIC seafile_common OpenSSL::Crypto PkgConfig::GLIB)
target_compile_options(cdc PRIVATE -Wall)

function(seafile_executable target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${GENERATED_DIR}")
    target_compile_definitions(${target} PRIVATE SEAFILE_SERVER)
    target_compile_options(${target} PRIVATE -Wall)
    target_link_libraries(${target} PRIVATE
        seafile_common PkgConfig::GLIB PkgConfig::GOBJECT searpc_deps
        PkgConfig::JANSSON PkgConfig::LIBEVENT OpenSSL::SSL OpenSSL::Crypto
        SQLite::SQLite3 Threads::Threads ZLIB::ZLIB PkgConfig::UUID)
    install(TARGETS ${target} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endfunction()

if(SEAFILE_BUILD_SERVER)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET "libcurl>=7.17")
    pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET "hiredis>=0.15.0")
    pkg_check_modules(JWT REQUIRED IMPORTED_TARGET libjwt)
    pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)
    pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET "libarchive>=2.8.5")
    pkg_check_modules(MYSQL QUIET IMPORTED_TARGET mysqlclient)

    set(SERVER_COMMON_SOURCES
        common/seaf-db.c common/branch-mgr.c common/fs-mgr.c common/config-mgr.c
        common/commit-mgr.c common/log.c common/object-list.c common/vc-common.c
        common/seaf-utils.c common/obj-store.c common/obj-backend-fs.c
        common/seafile-crypt.c common/password-hash.c common/diff-simple.c
        common/mq-mgr.c common/user-mgr.c common/group-mgr.c common/org-mgr.c
        common/block-mgr.c common/block-backend.c common/block-backend-fs.c
        common/merge-new.c common/obj-cache.c common/redis-cache.c
        common/block-tx-utils.c)

    seafile_executable(seaf-server
        ${SEARPC_HEADERS}
        server/seaf-server.c server/web-accesstoken-mgr.c server/seafile-session.c
        server/zip-download-mgr.c server/index-blocks-mgr.c server/share-mgr.c
        server/passwd-mgr.c server/quota-mgr.c server/repo-op.c server/repo-perm.c
        server/size-sched.c server/virtual-repo.c server/copy-mgr.c
        server/http-server.c server/upload-file.c server/access-file.c
        server/pack-dir.c server/fileserver-config.c server/http-tx-mgr.c
        server/notif-mgr.c server/change-set.c server/metric-mgr.c
        server/filelock-mgr.c server/repo-mgr.c common/rpc-service.c
        ${SERVER_COMMON_SOURCES})
    target_include_directories(seaf-server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/server")
    target_compile_definitions(seaf-server PRIVATE
        FULL_FEATURE
        PKGDATADIR="${CMAKE_INSTALL_FULL_DATADIR}/seafile"
        PACKAGE_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/seafile")
    target_link_libraries(seaf-server PRIVATE cdc PkgConfig::CURL PkgConfig::HIREDIS
        PkgConfig::JWT PkgConfig::ARGON2 PkgConfig::LIBARCHIVE)

    if(MYSQL_FOUND)
        target_compile_definitions(seaf-server PRIVATE HAVE_MYSQL)
        target_link_libraries(seaf-server PRIVATE PkgConfig::MYSQL)
    endif()

    if(SEAFILE_ENABLE_HTTPSERVER)
        find_path(EVHTP_INCLUDE_DIR evhtp.h REQUIRED)
        find_library(EVHTP_LIBRARY evhtp REQUIRED)
        target_compile_definitions(seaf-server PRIVATE HAVE_EVHTP)
        target_include_directories(seaf-server PRIVATE "${EVHTP_INCLUDE_DIR}")
        target_link_libraries(seaf-server PRIVATE "${EVHTP_LIBRARY}")
    endif()

    set(GC_COMMON_SOURCES
        server/gc/seafile-session.c server/gc/repo-mgr.c
        common/seaf-db.c common/branch-mgr.c common/fs-mgr.c common/block-mgr.c
        common/block-backend.c common/block-backend-fs.c common/commit-mgr.c
        common/log.c common/seaf-utils.c common/obj-store.c common/obj-backend-fs.c
        common/seafile-crypt.c common/password-hash.c common/config-mgr.c)

    seafile_executable(seafserv-gc
        server/gc/seafserv-gc.c server/gc/verify.c server/gc/gc-core.c ${GC_COMMON_SOURCES})
    seafile_executable(seaf-fsck
        server/gc/seaf-fsck.c server/gc/fsck.c ${GC_COMMON_SOURCES})
    foreach(target seafserv-gc seaf-fsck)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/server/gc")
        target_link_libraries(${target} PRIVATE cdc PkgConfig::JWT PkgConfig::ARGON2)
        if(MYSQL_FOUND)
            target_compile_definitions(${target} PRIVATE HAVE_MYSQL)
            target_link_libraries(${target} PRIVATE PkgConfig::MYSQL)
        endif()
    endforeach()
endif()

if(SEAFILE_BUILD_CONTROLLER AND NOT WIN32)
    seafile_executable(seafile-controller controller/seafile-controller.c common/log.c)
    target_include_directories(seafile-controller PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/controller")
endif()

if(SEAFILE_BUILD_FUSE)
    pkg_check_modules(FUSE REQUIRED IMPORTED_TARGET "fuse>=2.7.3")
    pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)
    pkg_check_modules(JWT REQUIRED IMPORTED_TARGET libjwt)
    pkg_check_modules(MYSQL QUIET IMPORTED_TARGET mysqlclient)
    seafile_executable(seaf-fuse
        fuse/seaf-fuse.c fuse/seafile-session.c fuse/file.c fuse/getattr.c
        fuse/readdir.c fuse/repo-mgr.c common/block-mgr.c common/user-mgr.c
        common/group-mgr.c common/org-mgr.c common/block-backend.c
        common/block-backend-fs.c common/branch-mgr.c common/commit-mgr.c
        common/fs-mgr.c common/log.c common/seaf-db.c common/seaf-utils.c
        common/obj-store.c common/obj-backend-fs.c common/obj-backend-riak.c
        common/seafile-crypt.c common/password-hash.c)
    target_include_directories(seaf-fuse PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fuse")
    target_compile_definitions(seaf-fuse PRIVATE
        PKGDATADIR="${CMAKE_INSTALL_FULL_DATADIR}/seafile"
        PACKAGE_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/seafile")
    target_link_libraries(seaf-fuse PRIVATE cdc PkgConfig::FUSE PkgConfig::JWT PkgConfig::ARGON2)
    if(MYSQL_FOUND)
        target_compile_definitions(seaf-fuse PRIVATE HAVE_MYSQL)
        target_link_libraries(seaf-fuse PRIVATE PkgConfig::MYSQL)
    endif()
endif()

install(FILES "${GENERATED_DIR}/seafile-object.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/seafile")
install(PROGRAMS tools/seafile-admin scripts/parse_seahub_db.py
    DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(SEAFILE_INSTALL_PYTHON)
    set(SEAFILE_PYTHON_INSTALL_DIR
        "${CMAKE_INSTALL_LIBDIR}/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages"
        CACHE PATH "Python module installation directory")
    install(DIRECTORY python/seafile python/seaserv
        DESTINATION "${SEAFILE_PYTHON_INSTALL_DIR}"
        FILES_MATCHING PATTERN "*.py")
endif()
