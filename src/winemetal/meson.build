winemetal_src = [
  'main.c',
  'airconv_thunks.c',
  'winemetal_thunks.c'
]

winemetal_ld_args = []

winemetal_link_depends = []

wine_build_path = get_option('wine_build_path')
wine_install_path = get_option('wine_install_path')
if wine_build_path != ''
  if not wine_build_path.startswith('/')
    wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
  endif
  lib_winecrt0 = cc.find_library('winecrt0', dirs: [
      join_paths(wine_build_path, 'libs/winecrt0', windows_builtin_install_dir),
      join_paths(wine_build_path, 'dlls/winecrt0', windows_builtin_install_dir),
    ],
  )
  lib_ntdll = cc.find_library('ntdll', dirs : [ join_paths(wine_build_path, 'dlls/ntdll', windows_builtin_install_dir)], static: true)
  lib_dbghelp = cc.find_library('dbghelp', dirs : [ join_paths(wine_build_path, 'dlls/dbghelp', windows_builtin_install_dir) ])
  winebuild = find_program(join_paths(wine_build_path, 'tools/winebuild/winebuild'), 'winebuild')
elif wine_install_path != ''
  if not wine_install_path.startswith('/')
    wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
  endif
  lib_winecrt0 = cc.find_library('winecrt0', dirs : [ join_paths(wine_install_path,'lib/wine', windows_builtin_install_dir) ])
  lib_ntdll = cc.find_library('ntdll', dirs : [ join_paths(wine_install_path, 'lib/wine', windows_builtin_install_dir)], static: true)
  lib_dbghelp = cc.find_library('dbghelp', dirs : [ join_paths(wine_install_path, 'lib/wine', windows_builtin_install_dir) ])
  winebuild = find_program(join_paths(wine_install_path, 'bin/winebuild'), 'winebuild')
else
  error('One of the options -Dwine_build_path or -Dwine_install_path is required')
endif

# gw2 (#182): athei/dxmt fc63ce8. winemetal takes only the unix call setup (unix_lib.o) from
# winecrt0. An ARM64EC image gets its CHPE metadata from libmingwex's chpe.o, which also defines
# __icall_helper_arm64ec. Since Wine 11.21 (Mach9's engine) winebuild generates the CHPE metadata
# and winecrt0's arm64ec.o defines only __icall_helper_arm64ec, so with winecrt0 searched first
# athei's link against Wine's build tree took both and failed on the symbol defined twice. Search
# libmingwex first: chpe.o is then the only provider, as for the DLLs that don't link winecrt0, and
# the link is the one Mach9's own winemetal.dll gets. With an older Wine, chpe.o provides it too.
winemetal_crt_deps = []
if cpu_family == 'aarch64'
  winemetal_crt_deps += cc.find_library('mingwex')
endif

winemetal_dll = shared_library('winemetal', winemetal_src,
  name_prefix         : '',
  dependencies        : [ winemetal_crt_deps, lib_winecrt0, lib_ntdll, lib_dbghelp, airconv_forward_dep ],
  include_directories : dxmt_include_path,
  install             : true,
  install_dir         : windows_builtin_install_dir,
  link_args           : winemetal_ld_args,
  link_depends        : [ winemetal_link_depends ],
)

winemetal_dep = declare_dependency(
  link_with           : [ winemetal_dll ],
  include_directories : [ include_directories('.') ],
)

custom_target('postprocess_lib',
  input : winemetal_dll,
  output: 'winemetal.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : winemetal_dll,
  build_by_default : true
)

if cpu_family == 'x86_64' or cpu_family == 'aarch64'
  subdir('unix')
endif
