set(FORKMESH_PACKAGE_MODULE_API 1)
if(UNIX AND NOT APPLE)
    set(_forkmesh_package_modules_default ON)
else()
    set(_forkmesh_package_modules_default OFF)
endif()
option(FORKMESH_PACKAGE_MODULES
    "Build every selected plugin the core does not link as its own lazily loaded module"
    ${_forkmesh_package_modules_default})

function(forkmesh_package_module_sources out id)
    forkmesh_package_variable_id(_variable "${id}")
    set(${out} "${FORKMESH_PACKAGE_${_variable}_SOURCES}" PARENT_SCOPE)
endfunction()

function(forkmesh_package_variable_id out id)
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" _variable "${id}")
    string(TOUPPER "${_variable}" _variable)
    set(${out} "${_variable}" PARENT_SCOPE)
endfunction()

function(forkmesh_linked_plugin_resources out)
    set(_qrcs "")
    if(NOT FORKMESH_PACKAGE_MODULES)
        foreach(_id IN LISTS FORKMESH_PLUGIN_MODULE_CANDIDATES)
            forkmesh_package_variable_id(_variable "${_id}")
            if(DEFINED FORKMESH_PACKAGE_${_variable}_QRC)
                list(APPEND _qrcs "${FORKMESH_PACKAGE_${_variable}_QRC}")
            endif()
        endforeach()
    endif()
    set(${out} "${_qrcs}" PARENT_SCOPE)
endfunction()

function(forkmesh_package_modules_partition)
    set(_modules "")
    set(_module_sources "")
    set(_module_headers "")
    if(FORKMESH_PACKAGE_MODULES)
        foreach(_id IN LISTS FORKMESH_PLUGIN_MODULE_CANDIDATES)
            forkmesh_package_variable_id(_variable "${_id}")
            if(NOT DEFINED FORKMESH_PACKAGE_${_variable}_SOURCES)
                continue()
            endif()
            list(APPEND _modules "${_id}")
            forkmesh_package_module_sources(_sources "${_id}")
            list(APPEND _module_sources ${_sources})
            foreach(_header IN LISTS FORKMESH_MARKETPLACE_MOC_HEADERS)
                if(_header MATCHES "/packages/${_id}/")
                    list(APPEND _module_headers "${_header}")
                endif()
            endforeach()
        endforeach()
    endif()
    set(_app_include_dirs "")
    foreach(_id IN LISTS FORKMESH_CORE_LINKED_PLUGINS)
        forkmesh_package_variable_id(_variable "${_id}")
        list(APPEND _app_include_dirs ${FORKMESH_PACKAGE_${_variable}_INCLUDE_DIRS})
    endforeach()
    foreach(_id IN LISTS FORKMESH_PLUGIN_MODULE_CANDIDATES)
        forkmesh_package_variable_id(_variable "${_id}")
        if(DEFINED FORKMESH_PACKAGE_${_variable}_SOURCES)
            set_source_files_properties(${FORKMESH_PACKAGE_${_variable}_SOURCES} PROPERTIES
                INCLUDE_DIRECTORIES "${FORKMESH_PACKAGE_${_variable}_INCLUDE_DIRS}")
        endif()
    endforeach()
    set(_app_sources ${FORKMESH_APP_SOURCES})
    set(_app_headers ${FORKMESH_MARKETPLACE_MOC_HEADERS})
    if(_module_sources)
        list(REMOVE_ITEM _app_sources ${_module_sources})
    endif()
    if(_module_headers)
        list(REMOVE_ITEM _app_headers ${_module_headers})
    endif()
    set(FORKMESH_BUILT_PACKAGE_MODULES "${_modules}" PARENT_SCOPE)
    set(FORKMESH_APP_EXECUTABLE_SOURCES "${_app_sources}" PARENT_SCOPE)
    set(FORKMESH_APP_MARKETPLACE_MOC_HEADERS "${_app_headers}" PARENT_SCOPE)
    set(FORKMESH_APP_MARKETPLACE_INCLUDE_DIRS "${_app_include_dirs}" PARENT_SCOPE)
endfunction()

function(forkmesh_package_module_abi out)
    set(${out} "${FORKMESH_VERSION}|${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}|Qt${Qt6_VERSION}|${CMAKE_BUILD_TYPE}" PARENT_SCOPE)
endfunction()

function(forkmesh_add_package_modules)
    forkmesh_package_module_abi(_abi)
    include(GNUInstallDirs)
    file(RELATIVE_PATH _relative "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}/forkmesh/packages")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/PackageModules.cpp" TARGET_DIRECTORY forkmesh
        APPEND PROPERTY COMPILE_DEFINITIONS
        FORKMESH_PACKAGE_MODULE_API=${FORKMESH_PACKAGE_MODULE_API}
        "FORKMESH_PACKAGE_MODULE_ABI=\"${_abi}\""
        "FORKMESH_PACKAGE_MODULE_RELDIR=\"${_relative}\"")
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/packages")
    file(GLOB _existing "${_output}/*.json" "${_output}/*${CMAKE_SHARED_MODULE_SUFFIX}")
    foreach(_file IN LISTS _existing)
        get_filename_component(_stem "${_file}" NAME_WE)
        if(NOT _stem IN_LIST FORKMESH_BUILT_PACKAGE_MODULES AND NOT _stem STREQUAL "web-engine")
            file(REMOVE "${_file}")
        endif()
    endforeach()
    if(NOT FORKMESH_BUILT_PACKAGE_MODULES)
        return()
    endif()
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/package-modules")
    file(MAKE_DIRECTORY "${_output}")
    foreach(_id IN LISTS FORKMESH_BUILT_PACKAGE_MODULES)
        forkmesh_package_variable_id(_variable "${_id}")
        set(_target "forkmesh-package-${_id}")
        set(_entrypoints "[]")
        set(_manifest "${CMAKE_CURRENT_SOURCE_DIR}/../marketplace/packages/${_id}/manifest.json")
        if(EXISTS "${_manifest}")
            file(READ "${_manifest}" _manifest_json)
            string(JSON _desktop ERROR_VARIABLE _missing GET "${_manifest_json}" entrypoints desktop)
            if(NOT _missing)
                set(_entrypoints "[\"${_desktop}\"]")
            endif()
        endif()
        set(_headers "")
        foreach(_header IN LISTS FORKMESH_MARKETPLACE_MOC_HEADERS)
            if(_header MATCHES "/packages/${_id}/")
                list(APPEND _headers "${_header}")
            endif()
        endforeach()
        file(WRITE "${_generated}/${_id}/module.cpp.in"
"extern \"C\" __attribute__((visibility(\"default\"))) int forkmesh_package_module_api() { return ${FORKMESH_PACKAGE_MODULE_API}; }
extern \"C\" __attribute__((visibility(\"default\"))) const char *forkmesh_package_module_abi() { return \"${_abi}\"; }
extern \"C\" __attribute__((visibility(\"default\"))) const char *forkmesh_package_module_id() { return \"${_id}\"; }
")
        configure_file("${_generated}/${_id}/module.cpp.in" "${_generated}/${_id}/module.cpp" COPYONLY)
        forkmesh_package_module_sources(_sources "${_id}")
        set(_resources "")
        if(DEFINED FORKMESH_PACKAGE_${_variable}_QRC)
            qt_add_resources(_resources "${FORKMESH_PACKAGE_${_variable}_QRC}")
        endif()
        add_library(${_target} MODULE
            ${_sources}
            ${_resources}
            "${_generated}/${_id}/module.cpp")
        if(_headers)
            set(_mocs "")
            qt_wrap_cpp(_mocs ${_headers} TARGET ${_target})
            target_sources(${_target} PRIVATE ${_mocs})
            set_source_files_properties(${_headers} PROPERTIES SKIP_AUTOMOC OFF)
            set(_mocs "")
        endif()
        set_target_properties(${_target} PROPERTIES
            PREFIX ""
            OUTPUT_NAME "${_id}"
            LIBRARY_OUTPUT_DIRECTORY "${_output}"
            AUTOMOC OFF
            JOB_POOL_COMPILE forkmesh_heavy)
        target_include_directories(${_target} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(${_target} PRIVATE
            "$<TARGET_PROPERTY:forkmesh,COMPILE_DEFINITIONS>")
        target_compile_options(${_target} PRIVATE
            "$<TARGET_PROPERTY:forkmesh,COMPILE_OPTIONS>")
        target_link_libraries(${_target} PRIVATE
            Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Concurrent OpenSSL::Crypto)
        forkmesh_link_multimedia(${_target})
        forkmesh_link_webengine(${_target})
        forkmesh_link_opengl(${_target})
        forkmesh_link_webp(${_target})
        add_dependencies(${_target} forkmesh-version-header)
        add_dependencies(forkmesh ${_target})
        file(WRITE "${_output}/${_id}.json"
            "{\"api\":${FORKMESH_PACKAGE_MODULE_API},\"abi\":\"${_abi}\",\"package\":\"${_id}\",\"file\":\"${_id}${CMAKE_SHARED_MODULE_SUFFIX}\",\"entrypoints\":${_entrypoints}}\n")
        if(APPLE)
            install(TARGETS ${_target} LIBRARY DESTINATION "ForkMesh.app/Contents/PlugIns/forkmesh-packages")
            install(FILES "${_output}/${_id}.json" DESTINATION "ForkMesh.app/Contents/PlugIns/forkmesh-packages")
        else()
            install(TARGETS ${_target} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/forkmesh/packages")
            install(FILES "${_output}/${_id}.json" DESTINATION "${CMAKE_INSTALL_LIBDIR}/forkmesh/packages")
        endif()
    endforeach()
    message(STATUS "ForkMesh package modules: ${FORKMESH_BUILT_PACKAGE_MODULES}")
endfunction()

function(forkmesh_add_web_engine_module)
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/packages")
    set(_built OFF)
    if(TARGET Qt6::WebEngineWidgets)
        target_compile_definitions(forkmesh PRIVATE FORKMESH_WEB_ENGINE_BUILT)
        if(FORKMESH_PACKAGE_MODULES)
            set(_built ON)
        else()
            forkmesh_embed_web_engine(forkmesh)
        endif()
    endif()
    if(NOT _built)
        file(REMOVE "${_output}/web-engine.json" "${_output}/web-engine${CMAKE_SHARED_MODULE_SUFFIX}")
        return()
    endif()
    forkmesh_package_module_abi(_abi)
    include(GNUInstallDirs)
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/package-modules/web-engine")
    file(MAKE_DIRECTORY "${_output}" "${_generated}")
    file(WRITE "${_generated}/module.cpp.in"
"extern \"C\" __attribute__((visibility(\"default\"))) int forkmesh_package_module_api() { return ${FORKMESH_PACKAGE_MODULE_API}; }
extern \"C\" __attribute__((visibility(\"default\"))) const char *forkmesh_package_module_abi() { return \"${_abi}\"; }
extern \"C\" __attribute__((visibility(\"default\"))) const char *forkmesh_package_module_id() { return \"web-engine\"; }
")
    configure_file("${_generated}/module.cpp.in" "${_generated}/module.cpp" COPYONLY)
    add_library(forkmesh-web-engine MODULE
        src/WebEngine.h
        src/WebEngineQt.cpp
        "${_generated}/module.cpp")
    set_target_properties(forkmesh-web-engine PROPERTIES
        PREFIX ""
        OUTPUT_NAME "web-engine"
        LIBRARY_OUTPUT_DIRECTORY "${_output}"
        AUTOMOC OFF)
    target_include_directories(forkmesh-web-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(forkmesh-web-engine PRIVATE
        "$<TARGET_PROPERTY:forkmesh,COMPILE_DEFINITIONS>")
    target_compile_options(forkmesh-web-engine PRIVATE
        "$<TARGET_PROPERTY:forkmesh,COMPILE_OPTIONS>")
    target_link_libraries(forkmesh-web-engine PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
    add_dependencies(forkmesh forkmesh-web-engine)
    file(WRITE "${_output}/web-engine.json"
        "{\"api\":${FORKMESH_PACKAGE_MODULE_API},\"abi\":\"${_abi}\",\"package\":\"web-engine\",\"file\":\"web-engine${CMAKE_SHARED_MODULE_SUFFIX}\",\"entrypoints\":[\"core:web-engine\"]}\n")
    if(APPLE)
        install(TARGETS forkmesh-web-engine LIBRARY DESTINATION "ForkMesh.app/Contents/PlugIns/forkmesh-packages")
        install(FILES "${_output}/web-engine.json" DESTINATION "ForkMesh.app/Contents/PlugIns/forkmesh-packages")
    else()
        install(TARGETS forkmesh-web-engine LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/forkmesh/packages")
        install(FILES "${_output}/web-engine.json" DESTINATION "${CMAKE_INSTALL_LIBDIR}/forkmesh/packages")
    endif()
    message(STATUS "Qt WebEngine: built as the web-engine module, loaded the first time a page opens")
endfunction()

function(forkmesh_add_package_module_tests)
    if(NOT FORKMESH_BUILT_PACKAGE_MODULES OR NOT UNIX OR APPLE)
        return()
    endif()
    add_test(NAME forkmesh-package-modules-resolve
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_package_modules.py"
                "$<TARGET_FILE_DIR:forkmesh>")
    add_test(NAME forkmesh-plugin-build-isolation
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_plugin_build_isolation.py"
                "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../marketplace/packages"
                "${CMAKE_MAKE_PROGRAM}")
endfunction()
