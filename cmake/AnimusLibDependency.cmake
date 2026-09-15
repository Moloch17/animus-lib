# animus-lib as a dependency of another AzerothCore module (mod-animus, mod-animus-forge).
#
# A dependent's <module>.cmake clones the library into modules/mod-animus-lib when it is missing, includes this file
# and calls AnimusLibRequire(<dependent>). modules/CMakeLists.txt runs <module>.cmake files after the `modules` target
# exists, in its scope, so this sees the configure's module list and each module's linkage (static, dynamic, disabled).
#
# - The library is a module of this configure: static builds need nothing more (every static module is in `modules`);
#   a dynamic dependent links the library's own shared module, so the library must be dynamic too.
# - The library was cloned during this configure: its sources and include directories are added to `modules` here
#   (static dependents only), and the dependents' loaders run its scripts (Addmod_animus_libScripts is idempotent).
#   The next configure finds it as a module like any other.

set(ANIMUS_LIB_MODULE "mod-animus-lib")
set(ANIMUS_LIB_DIR "${CMAKE_SOURCE_DIR}/modules/${ANIMUS_LIB_MODULE}")

function(AnimusLibRequire dependent)
  ModuleNameToVariable(${dependent} dependentVariable)
  set(dependentLinkage "${${dependentVariable}}")
  if(NOT dependentLinkage MATCHES "static|dynamic")
    return()
  endif()

  string(TOLOWER "mod_${dependent}" dependentProject)
  string(TOLOWER "mod_${ANIMUS_LIB_MODULE}" libraryProject)

  list(FIND MODULES_MODULE_LIST ${ANIMUS_LIB_MODULE} libraryIndex)
  if(NOT libraryIndex EQUAL -1)
    ModuleNameToVariable(${ANIMUS_LIB_MODULE} libraryVariable)
    set(libraryLinkage "${${libraryVariable}}")

    if(NOT libraryLinkage MATCHES "static|dynamic")
      message(FATAL_ERROR "${dependent} needs ${ANIMUS_LIB_MODULE}, which is disabled: set ${libraryVariable} to "
        "${dependentLinkage} or disable ${dependent}")
    endif()

    if(NOT libraryLinkage STREQUAL dependentLinkage)
      message(FATAL_ERROR "${dependent} is built ${dependentLinkage} but ${ANIMUS_LIB_MODULE} ${libraryLinkage}: "
        "build both the same way (${libraryVariable}=${dependentLinkage})")
    endif()

    if(dependentLinkage STREQUAL "dynamic")
      target_link_libraries(${dependentProject} PUBLIC ${libraryProject})
    endif()
    return()
  endif()

  if(dependentLinkage STREQUAL "dynamic")
    message(FATAL_ERROR "${ANIMUS_LIB_MODULE} was cloned during this configure; run cmake again so the dynamic "
      "${dependent} can link it")
  endif()

  get_property(alreadyAdded GLOBAL PROPERTY ANIMUS_LIB_ADDED_TO_MODULES)
  if(alreadyAdded)
    return()
  endif()
  set_property(GLOBAL PROPERTY ANIMUS_LIB_ADDED_TO_MODULES TRUE)

  CollectSourceFiles(${ANIMUS_LIB_DIR} librarySources)
  CollectIncludeDirectories(${ANIMUS_LIB_DIR} libraryIncludes)
  target_sources(modules PRIVATE ${librarySources})
  target_include_directories(modules PUBLIC ${libraryIncludes})
  message(STATUS "  ${ANIMUS_LIB_MODULE} added to the static modules for ${dependent}")
endfunction()
