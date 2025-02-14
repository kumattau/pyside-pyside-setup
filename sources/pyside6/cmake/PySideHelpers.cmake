macro(collect_essential_modules)
    # Collect all essential modules.
    # note: the order of this list is relevant for dependencies.
    # For instance: Qt5Printsupport must come before Qt5WebKitWidgets.
    set(ALL_ESSENTIAL_MODULES
        Core
        Gui
        Widgets
        PrintSupport
        Sql
        Network
        Test
        Concurrent)
endmacro()

macro(collect_optional_modules)
    # Collect all optional modules.
    set(ALL_OPTIONAL_MODULES
        Designer
        Xml
        Help Multimedia
        MultimediaWidgets
        OpenGL
        OpenGLWidgets
        Positioning
        Location
        NetworkAuth
        Qml
        Quick
        Quick3D
        QuickControls2
        QuickWidgets
        RemoteObjects
        Scxml
        Sensors
        SerialPort
        StateMachine
        TextToSpeech
        Charts
        Svg
        SvgWidgets
        DataVisualization
        Bluetooth)
    find_package(Qt${QT_MAJOR_VERSION}UiTools)
    if(Qt${QT_MAJOR_VERSION}UiTools_FOUND)
        list(APPEND ALL_OPTIONAL_MODULES UiTools)
    else()
        set(DISABLE_QtUiTools 1)
    endif()
    if(WIN32)
        list(APPEND ALL_OPTIONAL_MODULES AxContainer)
    endif()
    list(APPEND ALL_OPTIONAL_MODULES WebChannel WebEngineCore WebEngineWidgets
         WebEngineQuick WebSockets)
    if(NOT WIN32 AND NOT APPLE)
        list(APPEND ALL_OPTIONAL_MODULES DBus)
    endif()
    if (Qt${QT_MAJOR_VERSION}Core_VERSION VERSION_GREATER 6.0.2)
        list(APPEND ALL_OPTIONAL_MODULES 3DCore 3DRender 3DInput 3DLogic 3DAnimation 3DExtras)
    endif()
    if(WIN32)
        list(APPEND ALL_OPTIONAL_MODULES WinExtras)
    endif()
endmacro()

macro(check_os)
    set(ENABLE_UNIX "1")
    set(ENABLE_MAC "0")
    set(ENABLE_WIN "0")

    if(CMAKE_HOST_APPLE)
        set(ENABLE_MAC "1")
    elseif(CMAKE_HOST_WIN32)
        set(ENABLE_WIN "1")
        set(ENABLE_UNIX "0")
    elseif(NOT CMAKE_HOST_UNIX)
        message(FATAL_ERROR "OS not supported")
    endif()
endmacro()

macro(use_protected_as_public_hack)
    # 2017-04-24 The protected hack can unfortunately not be disabled, because
    # Clang does produce linker errors when we disable the hack.
    # But the ugly workaround in Python is replaced by a shiboken change.
    if(WIN32 OR DEFINED AVOID_PROTECTED_HACK)
        message(STATUS "PySide6 will be generated avoiding the protected hack!")
        set(GENERATOR_EXTRA_FLAGS ${GENERATOR_EXTRA_FLAGS} --avoid-protected-hack)
        add_definitions(-DAVOID_PROTECTED_HACK)
    else()
        message(STATUS "PySide will be generated using the protected hack!")
    endif()
endmacro()

macro(remove_skipped_modules)
    # Removing from the MODULES list the items that were defined with
    # -DSKIP_MODULES on command line
    if(SKIP_MODULES)
        foreach(s ${SKIP_MODULES})
            list(REMOVE_ITEM MODULES ${s})
        endforeach()
    endif()

    foreach(m ${MODULES})
        collect_module_if_found(${m})
        list(FIND all_module_shortnames ${m} is_module_collected)
        # If the module was collected, remove it from disabled modules list.
        if (NOT is_module_collected EQUAL -1)
            list(REMOVE_ITEM DISABLED_MODULES ${m})
        endif()
    endforeach()
endmacro()

macro(collect_module_if_found shortname)
    set(name "Qt${QT_MAJOR_VERSION}${shortname}")
    set(_qt_module_name "${name}")
    if ("${shortname}" STREQUAL "OpenGLFunctions")
        set(_qt_module_name "Qt${QT_MAJOR_VERSION}Gui")
    endif()
    # Determine essential/optional/missing
    set(module_state "missing")
    list(FIND ALL_ESSENTIAL_MODULES "${shortname}" essentialIndex)
    if(${essentialIndex} EQUAL -1)
        list(FIND ALL_OPTIONAL_MODULES "${shortname}" optionalIndex)
        if(NOT ${optionalIndex} EQUAL -1)
            set(module_state "optional")
        endif()
    else()
        set(module_state "essential")
    endif()

    # Silence warnings when optional packages are not found when doing a quiet build.
    set(quiet_argument "")
    if (QUIET_BUILD AND "${module_state}" STREQUAL "optional")
        set(quiet_argument "QUIET")
    endif()

    find_package(${_qt_module_name} ${quiet_argument})
    # If package is found, _name_found will be equal to 1
    set(_name_found "${_qt_module_name}_FOUND")
    # _name_dir will keep the path to the directory where the CMake rules were found
    # e.g: ~/qt5.9-install/qtbase/lib/cmake/Qt5Core or /usr/lib64/cmake/Qt5Core
    set(_name_dir "${_qt_module_name}_DIR")
    # Qt5Core will set the base path to check if all the modules are on the same
    # directory, to avoid CMake looking in another path.
    # This will be saved in a global variable at the beginning of the modules
    # collection process.
    string(FIND "${name}" "Qt${QT_MAJOR_VERSION}Core" qtcore_found)
    if(("${qtcore_found}" GREATER "0") OR ("${qtcore_found}" EQUAL "0"))
        get_filename_component(_core_abs_dir "${${_name_dir}}/../" ABSOLUTE)
        # Setting the absolute path where the Qt5Core was found
        # e.g: ~/qt5.9-install/qtbase/lib/cmake or /usr/lib64/cmake
        message(STATUS "CORE_ABS_DIR:" ${_core_abs_dir})
    endif()

    # Getting the absolute path for each module where the CMake was found, to
    # compare it with CORE_ABS_DIR and check if they are in the same source directory
    # e.g: ~/qt5.9-install/qtbase/lib/cmake/Qt5Script or /usr/lib64/cmake/Qt5Script
    get_filename_component(_module_dir "${${_name_dir}}" ABSOLUTE)
    string(FIND "${_module_dir}" "${_core_abs_dir}" found_basepath)

    # If the module was found, and also the module path is the same as the
    # Qt5Core base path, we will generate the list with the modules to be installed
    set(looked_in_message ". Looked in: ${${_name_dir}}")
    if("${${_name_found}}" AND (("${found_basepath}" GREATER "0") OR ("${found_basepath}" EQUAL "0")))
        message(STATUS "${module_state} module ${name} found (${ARGN})${looked_in_message}")
        # record the shortnames for the tests
        list(APPEND all_module_shortnames ${shortname})
        # Build Qt 5 compatibility variables
        if(${QT_MAJOR_VERSION} GREATER_EQUAL 6 AND NOT "${shortname}" STREQUAL "OpenGLFunctions")
            get_target_property(Qt6${shortname}_INCLUDE_DIRS Qt6::${shortname}
                                INTERFACE_INCLUDE_DIRECTORIES)
            get_target_property(Qt6${shortname}_PRIVATE_INCLUDE_DIRS
                                Qt6::${shortname}Private
                                INTERFACE_INCLUDE_DIRECTORIES)
            set(Qt6${shortname}_LIBRARIES Qt::${shortname})
        endif()
    else()
        if("${module_state}" STREQUAL "optional")
            message(STATUS "optional module ${name} skipped${looked_in_message}")
        elseif("${module_state}" STREQUAL "essential")
            message(STATUS "skipped module ${name} is essential!\n"
                           "   We do not guarantee that all tests are working.${looked_in_message}")
        else()
            message(FATAL_ERROR "module ${name} MISSING${looked_in_message}")
        endif()
    endif()
endmacro()
