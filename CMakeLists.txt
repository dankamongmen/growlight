cmake_minimum_required(VERSION 3.14)
project(growlight VERSION 1.2.6
  DESCRIPTION "Block device and filesystem manager"
  HOMEPAGE_URL "https://nick-black.com/dankwiki/index.php/Growlight"
  LANGUAGES C)
set(CMAKE_C_STANDARD 11)

include(GNUInstallDirs)

###################### USER-SELECTABLE OPTIONS ###########################
option(USE_DOCBOOK "Use docbook to write man pages" ON)
option(USE_LIBZFS "Use libzfs to manage zpools/ZFS" ON)
#################### END USER-SELECTABLE OPTIONS #########################

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the build mode." FORCE)
endif()

add_compile_definitions(FORTIFY_SOURCE=2)
add_compile_options(-Wall -Wextra -W -Wshadow -Wformat -fexceptions)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(notcurses REQUIRED)
pkg_check_modules(LIBATASMART REQUIRED libatasmart>=0.19)
pkg_check_modules(LIBBLKID REQUIRED blkid>=2.20.1)
pkg_check_modules(LIBCRYPTSETUP REQUIRED libcryptsetup>=2.0.2)
pkg_check_modules(LIBDEVMAPPER REQUIRED devmapper>=1.02.74)
pkg_check_modules(LIBNETTLE REQUIRED nettle>=3.5.1)
pkg_check_modules(LIBPCI REQUIRED libpci>=3.1.9)
pkg_check_modules(LIBPCIACCESS REQUIRED pciaccess>=0.13.1)
pkg_check_modules(LIBREADLINE REQUIRED readline)
pkg_check_modules(LIBUDEV REQUIRED libudev>=175)
pkg_check_modules(LIBZFS REQUIRED libzfs>=0.8)

include(CMakePackageConfigHelpers)
configure_file(src/version.h.in include/version.h)

file(GLOB SRCS CONFIGURE_DEPENDS src/*.c)
file(GLOB NCSRCS CONFIGURE_DEPENDS src/notcurses/*.c)
add_executable(growlight ${SRCS} ${NCSRCS})
target_include_directories(growlight
  PRIVATE
    src
    "${PROJECT_BINARY_DIR}/include"
    "${LIBATASMART_INCLUDE_DIRS}"
    "${LIBBLKID_INCLUDE_DIRS}"
    "${LIBCRYPTSETUP_INCLUDE_DIRS}"
    "${LIBDEVMAPPER_INCLUDE_DIRS}"
    "${LIBNETTLE_INCLUDE_DIRS}"
    "${LIBPCI_INCLUDE_DIRS}"
    "${LIBPCIACCESS_INCLUDE_DIRS}"
    "${LIBUDEV_INCLUDE_DIRS}"
    "${LIBZFS_INCLUDE_DIRS}"
    "${notcurses_INCLUDE_DIRS}"
)
target_compile_definitions(growlight
  PRIVATE
    _GNU_SOURCE _DEFAULT_SOURCE _XOPEN_SOURCE=600
)
target_link_libraries(growlight
  PRIVATE
    "${LIBATASMART_LIBRARIES}"
    "${LIBBLKID_LIBRARIES}"
    "${LIBCRYPTSETUP_LIBRARIES}"
    "${LIBDEVMAPPER_LIBRARIES}"
    "${LIBNETTLE_LIBRARIES}"
    "${LIBPCI_LIBRARIES}"
    "${LIBPCIACCESS_LIBRARIES}"
    "${LIBUDEV_LIBRARIES}"
    "${LIBZFS_LIBRARIES}"
    "${notcurses_LIBRARIES}"
    Threads::Threads
)
target_link_directories(growlight
  PRIVATE
    "${LIBATASMART_LIBRARY_DIRS}"
    "${LIBBLKID_LIBRARY_DIRS}"
    "${LIBCRYPTSETUP_LIBRARY_DIRS}"
    "${LIBDEVMAPPER_LIBRARY_DIRS}"
    "${LIBNETTLE_LIBRARY_DIRS}"
    "${LIBPCI_LIBRARY_DIRS}"
    "${LIBPCIACCESS_LIBRARY_DIRS}"
    "${LIBUDEV_LIBRARY_DIRS}"
    "${LIBZFS_LIBRARY_DIRS}"
    "${notcurses_LIBRARY_DIRS}"
)
file(GLOB RLSRCS CONFIGURE_DEPENDS src/readline/*.c)
add_executable(growlight-readline ${SRCS} ${RLSRCS})
target_include_directories(growlight-readline
  PRIVATE
    src
    "${PROJECT_BINARY_DIR}/include"
    "${LIBATASMART_INCLUDE_DIRS}"
    "${LIBBLKID_INCLUDE_DIRS}"
    "${LIBCRYPTSETUP_INCLUDE_DIRS}"
    "${LIBDEVMAPPER_INCLUDE_DIRS}"
    "${LIBNETTLE_INCLUDE_DIRS}"
    "${LIBPCI_INCLUDE_DIRS}"
    "${LIBPCIACCESS_INCLUDE_DIRS}"
    "${LIBREADLINE_INCLUDE_DIRS}"
    "${LIBUDEV_INCLUDE_DIRS}"
    "${LIBZFS_INCLUDE_DIRS}"
    "${notcurses_INCLUDE_DIRS}"
)
target_compile_definitions(growlight-readline
  PRIVATE
    _GNU_SOURCE _DEFAULT_SOURCE _XOPEN_SOURCE=600
)
target_link_libraries(growlight-readline
  PRIVATE
    "${LIBATASMART_LIBRARIES}"
    "${LIBBLKID_LIBRARIES}"
    "${LIBCRYPTSETUP_LIBRARIES}"
    "${LIBDEVMAPPER_LIBRARIES}"
    "${LIBNETTLE_LIBRARIES}"
    "${LIBPCI_LIBRARIES}"
    "${LIBPCIACCESS_LIBRARIES}"
    "${LIBREADLINE_LIBRARIES}"
    "${LIBUDEV_LIBRARIES}"
    "${LIBZFS_LIBRARIES}"
    "${notcurses_LIBRARIES}"
    Threads::Threads
)
target_link_directories(growlight-readline
  PRIVATE
    "${LIBATASMART_LIBRARY_DIRS}"
    "${LIBBLKID_LIBRARY_DIRS}"
    "${LIBCRYPTSETUP_LIBRARY_DIRS}"
    "${LIBDEVMAPPER_LIBRARY_DIRS}"
    "${LIBNETTLE_LIBRARY_DIRS}"
    "${LIBPCI_LIBRARY_DIRS}"
    "${LIBPCIACCESS_LIBRARY_DIRS}"
    "${LIBREADLINE_LIBRARY_DIRS}"
    "${LIBUDEV_LIBRARY_DIRS}"
    "${LIBZFS_LIBRARY_DIRS}"
    "${notcurses_INCLUDE_DIRS}"
)

if(USE_DOCBOOK)
  file(GLOB MANSOURCE CONFIGURE_DEPENDS doc/man/man8/*.xml)
  find_program(XSLTPROC xsltproc)
  if(NOT XSLTPROC)
    message(FATAL_ERROR "xsltproc not found. USE_DOCBOOK=OFF to disable.")
  else()
    foreach(m ${MANSOURCE})
      get_filename_component(me ${m} NAME_WLE)
      add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${me}
        DEPENDS ${m}
        COMMAND ${XSLTPROC}
        ARGS --nonet /usr/share/xml/docbook/stylesheet/docbook-xsl/manpages/docbook.xsl ${m}
        COMMENT "Building man page ${me}"
      )
      add_custom_target(${me}.man
        ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${me}
      )
    endforeach()
    foreach(m ${MANSOURCE})
      get_filename_component(me, ${m} NAME_WLE)
      LIST(APPEND MANPAGES ${CMAKE_CURRENT_BINARY_DIR}/${me})
    endforeach()
  endif()
endif()

install(TARGETS growlight DESTINATION bin)
install(TARGETS growlight-readline DESTINATION bin)
install(FILES ${MANPAGES} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/man/man8)