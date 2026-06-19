function(cu_pkg_define_version component_dir)
    # Local offline build shim for Espressif component-registry metadata.
    # The real component-manager helper reads idf_component.yml and injects
    # version macros. We only need those macros for local offline builds.
    get_filename_component(_component_name "${component_dir}" NAME)
    set(_version "0.0.0")
    if(EXISTS "${component_dir}/idf_component.yml")
        file(STRINGS "${component_dir}/idf_component.yml" _version_line REGEX "^version:[ ]*\"?[0-9]+\\.[0-9]+\\.[0-9]+\"?")
        if(_version_line)
            string(REGEX REPLACE "^version:[ ]*\"?([0-9]+\\.[0-9]+\\.[0-9]+)\"?.*$" "\\1" _version "${_version_line}")
        endif()
    endif()

    string(REPLACE "." ";" _version_parts "${_version}")
    list(GET _version_parts 0 _ver_major)
    list(GET _version_parts 1 _ver_minor)
    list(GET _version_parts 2 _ver_patch)

    string(TOUPPER "${_component_name}" _macro_prefix)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        ${_macro_prefix}_VER_MAJOR=${_ver_major}
        ${_macro_prefix}_VER_MINOR=${_ver_minor}
        ${_macro_prefix}_VER_PATCH=${_ver_patch}
    )
endfunction()
