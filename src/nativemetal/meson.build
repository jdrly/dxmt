# gw2 (B1, #61): the unix sources and their link arguments come from src/winemetal/unix/meson.build,
# the list the Wine build uses (this list had lost pso_archive.c and mtl4.c)
winemetal_src = files('../winemetal/winemetal_thunks.c') + winemetal_unix_src
winemetal_link_depends = []

winemetal_ld_args = winemetal_unix_link_args

winemetal_dll = shared_library('winemetal', winemetal_src,
  name_prefix         : '',
  dependencies        : [ airconv_dep_darwin ],
  include_directories : [ dxmt_include_path, include_directories('.') ],
  c_args              : ['-ObjC', '-DDXMT_NATIVE=1'],
  link_args           : winemetal_ld_args,
  build_rpath         : '@loader_path/:@loader_path/../../',
)

winemetal_dep = declare_dependency(
  link_with           : [ winemetal_dll ],
  include_directories : [ include_directories('../winemetal') ],
)
