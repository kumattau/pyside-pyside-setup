# Don't ignore targets that do not exist, inside add_dependencies calls.
cmake_policy(SET CMP0046 NEW)

set(QT_MAJOR_VERSION 6)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../shiboken6/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/Macros")

# TODO: Don't directly include, ShibokenHelpers but rather pick it up from the installed Shiboken
# package. Needs to support top-level build as well (Shiboken is not yet installed in that case).
include(ShibokenHelpers)
include(PySideHelpers)

# Don't display "up-to-date / install" messages when installing, to reduce visual clutter.
if(QUIET_BUILD)
    set(CMAKE_INSTALL_MESSAGE NEVER)
endif()

# Override message not to display info messages when doing a quiet build.
if(QUIET_BUILD AND is_pyside6_superproject_build)
    function(message)
        list(GET ARGV 0 MessageType)
        if(MessageType STREQUAL FATAL_ERROR OR
            MessageType STREQUAL SEND_ERROR OR
            MessageType STREQUAL WARNING OR
            MessageType STREQUAL AUTHOR_WARNING)
                list(REMOVE_AT ARGV 0)
                _message(${MessageType} "${ARGV}")
        endif()
    endfunction()
endif()

find_package(Shiboken6 2.0.0 REQUIRED)

set(PYSIDE_VERSION_FILE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/pyside_version.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PYSIDE_VERSION_FILE_PATH}
)
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} "${PYSIDE_VERSION_FILE_PATH}"
  OUTPUT_VARIABLE PYSIDE_VERSION_OUTPUT
  ERROR_VARIABLE PYSIDE_VERSION_OUTPUT_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT PYSIDE_VERSION_OUTPUT)
    message(FATAL_ERROR "Could not identify PySide6 version. Error: ${PYSIDE_VERSION_OUTPUT_ERROR}")
endif()

# Detect if the Python interpreter is actually PyPy
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "if True:
        pypy_version = ''
        import sys
        if hasattr(sys, 'pypy_version_info'):
            pypy_version = '.'.join(map(str, sys.pypy_version_info[:3]))
        print(pypy_version)
        "
    OUTPUT_VARIABLE PYPY_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)

list(GET PYSIDE_VERSION_OUTPUT 0 BINDING_API_MAJOR_VERSION)
list(GET PYSIDE_VERSION_OUTPUT 1 BINDING_API_MINOR_VERSION)
list(GET PYSIDE_VERSION_OUTPUT 2 BINDING_API_MICRO_VERSION)
# a - alpha, b - beta, rc - rc
list(GET PYSIDE_VERSION_OUTPUT 3 BINDING_API_PRE_RELEASE_VERSION_TYPE)
# the number of the pre release (alpha1, beta3, rc7, etc.)
list(GET PYSIDE_VERSION_OUTPUT 4 BINDING_API_PRE_RELEASE_VERSION)

if(WIN32)
    set(PATH_SEP "\;")
else()
    set(PATH_SEP ":")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "${SHIBOKEN_BUILD_TYPE}" CACHE STRING "Build Type")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_definitions("-DNDEBUG")
endif()

if(SHIBOKEN_PYTHON_LIMITED_API)
    message(STATUS "******************************************************")
    message(STATUS "** PySide6 Limited API enabled.")
    message(STATUS "******************************************************")
endif()

find_package(Qt${QT_MAJOR_VERSION} 6.0 REQUIRED COMPONENTS Core)
add_definitions(${Qt${QT_MAJOR_VERSION}Core_DEFINITIONS})

option(BUILD_TESTS "Build tests." TRUE)
option(ENABLE_VERSION_SUFFIX "Used to use current version in suffix to generated files. This is used to allow multiples versions installed simultaneous." FALSE)
set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)" )
set(LIB_INSTALL_DIR "lib${LIB_SUFFIX}" CACHE PATH "The subdirectory relative to the install prefix where libraries will be installed (default is /lib${LIB_SUFFIX})" FORCE)
if(CMAKE_HOST_APPLE)
    set(ALTERNATIVE_QT_INCLUDE_DIR "" CACHE PATH "Deprecated. CMake now finds the proper include dir itself.")
    set(OSX_USE_LIBCPP "OFF" CACHE BOOL "Explicitly link the libc++ standard library (useful for osx deployment targets lower than 10.9.")
    if(OSX_USE_LIBCPP)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    endif()
endif()

# Force usage of the C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Obtain QT_INCLUDE_DIR from INTERFACE_INCLUDE_DIRECTORIES which returns a list
# ../install-qt-6/include/QtCore;../install-qt-6/include
set(QT_INCLUDE_DIR "")
get_target_property(QT_INCLUDE_DIR_LIST Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
foreach(_Q ${QT_INCLUDE_DIR_LIST})
    if(NOT "${_Q}" MATCHES "QtCore$")
        set(QT_INCLUDE_DIR "${_Q}")
    endif()
endforeach()
if(QT_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR "Unable to obtain the Qt include directory")
endif()

# On macOS, check if Qt is a framework build. This affects how include paths should be handled.
get_target_property(QtCore_is_framework Qt${QT_MAJOR_VERSION}::Core FRAMEWORK)

if(QtCore_is_framework)
    # Get the path to the framework dir.
    set(QT_FRAMEWORK_INCLUDE_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}")
    message(STATUS "*** QT_FRAMEWORK_INCLUDE_DIR is ${QT_FRAMEWORK_INCLUDE_DIR}")
    set(QT_INCLUDE_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_HEADERS}")
endif()

set_cmake_cxx_flags()

message(STATUS "*** computed QT_INCLUDE_DIR as ${QT_INCLUDE_DIR}")

set(BINDING_NAME PySide6)

set(BINDING_API_VERSION "${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION}.${BINDING_API_MICRO_VERSION}" CACHE STRING "PySide6 version" FORCE)
set(PYSIDE_SO_VERSION ${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION})
if(BINDING_API_PRE_RELEASE_VERSION_TYPE STREQUAL "")
    set(BINDING_API_VERSION_FULL "${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION}.${BINDING_API_MICRO_VERSION}"
        CACHE STRING "PySide6 version [full]" FORCE)
else()
    set(BINDING_API_VERSION_FULL "${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION}.${BINDING_API_MICRO_VERSION}~${BINDING_API_PRE_RELEASE_VERSION_TYPE}${BINDING_API_PRE_RELEASE_VERSION}"
        CACHE STRING "PySide6 version [full]" FORCE)
endif()

compute_config_py_values(BINDING_API_VERSION)

include(PySideModules)

# Set default values for pyside6_global.h
set (Qt${QT_MAJOR_VERSION}Test_FOUND "0")
set (Qt${QT_MAJOR_VERSION}Widgets_FOUND "0")

collect_essential_modules()
collect_optional_modules()

# Modules to be built unless specified by -DMODULES on command line
if(NOT MODULES)
    set(MODULES "${ALL_ESSENTIAL_MODULES};${ALL_OPTIONAL_MODULES}")
endif()

# This will contain the set of modules for which bindings are not built.
set(DISABLED_MODULES "${ALL_ESSENTIAL_MODULES};${ALL_OPTIONAL_MODULES}")

remove_skipped_modules()

# Mark all non-collected modules as disabled. This is used for disabling tests
# that depend on the disabled modules.
foreach(m ${DISABLED_MODULES})
    set(DISABLE_Qt${m} 1)
endforeach()


string(REGEX MATCHALL "[0-9]+" qt_version_helper "${Qt${QT_MAJOR_VERSION}Core_VERSION}")

list(GET qt_version_helper 0 QT_VERSION_MAJOR)
list(GET qt_version_helper 1 QT_VERSION_MINOR)
list(GET qt_version_helper 2 QT_VERSION_PATCH)
unset(qt_version_helper)

set(PYSIDE_QT_VERSION "${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}" CACHE STRING "Qt version used to compile PySide" FORCE)
if(ENABLE_VERSION_SUFFIX)
      set(pyside_SUFFIX "-${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION}")
endif()

# no more supported: include(${QT_USE_FILE})

# Configure OS support
check_os()

# Define supported Qt Version
set(SUPPORTED_QT_VERSION "${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}")


# uninstall target
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
               IMMEDIATE @ONLY)

# When opening super project, prevent redefinition of uninstall target.
if(NOT TARGET uninstall)
    add_custom_target(uninstall "${CMAKE_COMMAND}"
                      -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
endif()

if(NOT PYTHON_SITE_PACKAGES)
    execute_process(
        COMMAND ${SHIBOKEN_PYTHON_INTERPRETER} -c "if True:
            import sysconfig
            from os.path import sep

            # /home/qt/dev/env/lib/python3.9/site-packages
            lib_path = sysconfig.get_path('purelib')

            # /home/qt/dev/env
            data_path = sysconfig.get_path('data')

            # /lib/python3.9/site-packages
            rel_path = lib_path.replace(data_path, '')

            print(f'${CMAKE_INSTALL_PREFIX}{rel_path}'.replace(sep, '/'))
            "
        OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT PYTHON_SITE_PACKAGES)
        message(FATAL_ERROR "Could not detect Python module installation directory.")
    elseif(APPLE)
        message(STATUS "!!! The generated bindings will be installed on ${PYTHON_SITE_PACKAGES}, is it right!?")
    endif()
endif()

set(GENERATOR_EXTRA_FLAGS --generator-set=shiboken
                          --enable-parent-ctor-heuristic
                          --enable-pyside-extensions
                          --enable-return-value-heuristic
                          --use-isnull-as-nb_nonzero)
use_protected_as_public_hack()

# Build with Address sanitizer enabled if requested. This may break things, so use at your own risk.
if(SANITIZE_ADDRESS AND NOT MSVC)
    setup_sanitize_address()
endif()

find_package(Qt${QT_MAJOR_VERSION}Designer)

