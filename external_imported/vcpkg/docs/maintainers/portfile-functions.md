# Portfile helper functions
- [execute\_process](execute_process.md)
- [vcpkg\_acquire\_msys](vcpkg_acquire_msys.md)
- [vcpkg\_add\_to\_path](vcpkg_add_to_path.md)
- [vcpkg\_apply\_patches](vcpkg_apply_patches.md) (deprecated)
- [vcpkg\_backup\_restore\_env\_vars](vcpkg_backup_restore_env_vars.md)
- [vcpkg\_build\_cmake](vcpkg_build_cmake.md) (deprecated, use [vcpkg\_cmake\_build](vcpkg_cmake_build.md))
- [vcpkg\_build\_make](vcpkg_build_make.md)
- [vcpkg\_build\_msbuild](vcpkg_build_msbuild.md)
- [vcpkg\_build\_ninja](vcpkg_build_ninja.md)
- [vcpkg\_build\_nmake](vcpkg_build_nmake.md)
- [vcpkg\_build\_qmake](vcpkg_build_qmake.md)
- [vcpkg\_buildpath\_length\_warning](vcpkg_buildpath_length_warning.md)
- [vcpkg\_check\_features](vcpkg_check_features.md)
- [vcpkg\_check\_linkage](vcpkg_check_linkage.md)
- [vcpkg\_clean\_executables\_in\_bin](vcpkg_clean_executables_in_bin.md)
- [vcpkg\_clean\_msbuild](vcpkg_clean_msbuild.md)
- [vcpkg\_common\_definitions](vcpkg_common_definitions.md)
- [vcpkg\_configure\_cmake](vcpkg_configure_cmake.md) (deprecated, use [vcpkg\_cmake\_configure](vcpkg_cmake_configure.md))
- [vcpkg\_configure\_gn](vcpkg_configure_gn.md) (deprecated, use [vcpkg\_gn\_configure](ports/vcpkg-gn/vcpkg_gn_configure.md))
- [vcpkg\_configure\_make](vcpkg_configure_make.md)
- [vcpkg\_configure\_meson](vcpkg_configure_meson.md)
- [vcpkg\_configure\_qmake](vcpkg_configure_qmake.md)
- [vcpkg\_copy\_pdbs](vcpkg_copy_pdbs.md)
- [vcpkg\_copy\_tool\_dependencies](vcpkg_copy_tool_dependencies.md)
- [vcpkg\_copy\_tools](vcpkg_copy_tools.md)
- [vcpkg\_download\_distfile](vcpkg_download_distfile.md)
- [vcpkg\_execute\_build\_process](vcpkg_execute_build_process.md)
- [vcpkg\_execute\_in\_download\_mode](vcpkg_execute_in_download_mode.md)
- [vcpkg\_execute\_required\_process](vcpkg_execute_required_process.md)
- [vcpkg\_execute\_required\_process\_repeat](vcpkg_execute_required_process_repeat.md)
- [vcpkg\_extract\_source\_archive](vcpkg_extract_source_archive.md)
- [vcpkg\_extract\_source\_archive\_ex](vcpkg_extract_source_archive_ex.md)
- [vcpkg\_fail\_port\_install](vcpkg_fail_port_install.md) (deprecated)
- [vcpkg\_find\_acquire\_program](vcpkg_find_acquire_program.md)
- [vcpkg\_find\_fortran](vcpkg_find_fortran.md)
- [vcpkg\_fixup\_cmake\_targets](vcpkg_fixup_cmake_targets.md) (deprecated, use [vcpkg\_cmake\_config\_fixup](ports/vcpkg-cmake-config/vcpkg_cmake_config_fixup.md))
- [vcpkg\_fixup\_pkgconfig](vcpkg_fixup_pkgconfig.md)
- [vcpkg\_from\_bitbucket](vcpkg_from_bitbucket.md)
- [vcpkg\_from\_git](vcpkg_from_git.md)
- [vcpkg\_from\_github](vcpkg_from_github.md)
- [vcpkg\_from\_gitlab](vcpkg_from_gitlab.md)
- [vcpkg\_from\_sourceforge](vcpkg_from_sourceforge.md)
- [vcpkg\_get\_program\_files\_platform\_bitness](vcpkg_get_program_files_platform_bitness.md)
- [vcpkg\_get\_windows\_sdk](vcpkg_get_windows_sdk.md)
- [vcpkg\_host\_path\_list](vcpkg_host_path_list.md)
- [vcpkg\_install\_cmake](vcpkg_install_cmake.md) (deprecated, use [vcpkg\_cmake\_install](vcpkg_cmake_install.md))
- [vcpkg\_install\_gn](vcpkg_install_gn.md) (deprecated, use [vcpkg\_gn\_install](ports/vcpkg-gn/vcpkg_gn_install.md))
- [vcpkg\_install\_copyright](vcpkg_install_copyright.md)
- [vcpkg\_install\_make](vcpkg_install_make.md)
- [vcpkg\_install\_meson](vcpkg_install_meson.md)
- [vcpkg\_install\_msbuild](vcpkg_install_msbuild.md)
- [vcpkg\_install\_nmake](vcpkg_install_nmake.md)
- [vcpkg\_install\_qmake](vcpkg_install_qmake.md)
- [vcpkg\_list](vcpkg_list.md)
- [vcpkg\_minimum\_required](vcpkg_minimum_required.md)
- [vcpkg\_replace\_string](vcpkg_replace_string.md)

## Internal Functions

- [z\_vcpkg\_apply\_patches](internal/z_vcpkg_apply_patches.md)
- [z\_vcpkg\_forward\_output\_variable](internal/z_vcpkg_forward_output_variable.md)
- [z\_vcpkg\_function\_arguments](internal/z_vcpkg_function_arguments.md)
- [z\_vcpkg\_get\_cmake\_vars](internal/z_vcpkg_get_cmake_vars.md)
- [z\_vcpkg\_prettify\_command\_line](internal/z_vcpkg_prettify_command_line.md)
- [z\_vcpkg\_setup\_pkgconfig\_path](internal/z_vcpkg_setup_pkgconfig_path.md)

## Scripts from Ports

### [vcpkg-cmake](ports/vcpkg-cmake.md)

- [vcpkg\_cmake\_build](vcpkg_cmake_build.md)
- [vcpkg\_cmake\_configure](vcpkg_cmake_configure.md)
- [vcpkg\_cmake\_install](vcpkg_cmake_install.md)

### [vcpkg-gn](ports/vcpkg-gn.md)

- [vcpkg\_gn\_configure](ports/vcpkg-gn/vcpkg_gn_configure.md)
- [vcpkg\_gn\_install](ports/vcpkg-gn/vcpkg_gn_install.md)

### [vcpkg-cmake-config](ports/vcpkg-cmake-config.md)

- [vcpkg\_cmake\_config\_fixup](ports/vcpkg-cmake-config/vcpkg_cmake_config_fixup.md)

### [vcpkg-cmake-get-vars](ports/vcpkg-cmake-get-vars.md)

- [vcpkg\_cmake\_get\_vars](ports/vcpkg-cmake-get-vars/vcpkg_cmake_get_vars.md)

### [vcpkg-pkgconfig-get-modules](ports/vcpkg-pkgconfig-get-modules.md)

- [x\_vcpkg\_pkgconfig\_get\_modules](ports/vcpkg-pkgconfig-get-modules/x_vcpkg_pkgconfig_get_modules.md)

### [vcpkg-get-python-packages](ports/vcpkg-get-python-packages.md)

- [x\_vcpkg\_get\_python\_packages](ports/vcpkg-get-python-packages/x_vcpkg_get_python_packages.md)

### [vcpkg-qmake](ports/vcpkg-qmake.md)

- [vcpkg\_qmake\_configure](ports/vcpkg-qmake/vcpkg_qmake_configure.md)
