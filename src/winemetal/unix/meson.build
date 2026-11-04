# gw2 (B1, #61): the bridge's unix side, listed once. Both builds use this list: the Wine unix
# library below (the cross build) and src/nativemetal's dylib (the native build, DXMT_NATIVE). A new
# unix source goes here and nowhere else.
winemetal_unix_src = files(
  'winemetal_unix.c',
  'cache.c',
  'pso_archive.c',
  'mtl4.c', # gw2 (#32)
  'mtl4fx.c', # gw2 (#56)
)
# ... and the frameworks and libraries it links, for the same targets
winemetal_unix_link_args = [
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-framework', 'MetalFX', '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'ColorSync', '-weak_framework', 'Cocoa',
  '-lsqlite3',
]

# gw2 (B1, #61): the native smoke test. The PE side and the real unix side linked into one macOS
# executable with DXMT_NATIVE, so its calls reach __wine_unix_call_funcs directly. It is built with
# the x86_64 Wine build (a missing handler source fails the build) and is never installed; run it
# with `meson test -C <build> bridge-native-smoke` (suite gw2-metal).
bridge_native_smoke = executable('bridge_native_smoke',
  winemetal_unix_src,
  files('../winemetal_thunks.c', '../../../tests/gw2/bridge_native_smoke.c'),
  dependencies        : [ airconv_dep_darwin ],
  # nativemetal first: its wineunixlib.h (a direct table call), not src/winemetal's (Wine's)
  include_directories : [ dxmt_include_path, include_directories('../../nativemetal', '..') ],
  # -Wno-unused-function: DXMT_NATIVE compiles out the wow64 thunks, the only users of
  # winemetal_unix.c's UInt32ToPtr
  c_args              : ['-ObjC', '-DDXMT_NATIVE=1', '-Wno-unused-function'],
  link_args           : winemetal_unix_link_args,
  native              : true,
  install             : false,
)
test('bridge-native-smoke', bridge_native_smoke, suite : ['gw2', 'gw2-metal'])

if not dxmt_crossbuild
  subdir_done()
endif

if wine_build_path != ''
  winemac_so = join_paths(wine_build_path, 'dlls/winemac.drv/winemac.so')
  ntdll_so = join_paths(wine_build_path, 'dlls/ntdll/ntdll.so')
elif wine_install_path != ''
  winemac_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'winemac.so')
  ntdll_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'ntdll.so')
else
  error('One of the options -Dwine_build_path or -Dwine_install_path is required')
endif

winemetal_unix_ld_args = [
  '-Wl,-install_name,@rpath/winemetal.so',
  winemac_so, ntdll_so,
] + winemetal_unix_link_args
winemetal_unix_link_depends = []

shared_library('winemetal', winemetal_unix_src,
  name_prefix         : '',
  dependencies        : [ airconv_dep_darwin ],
  include_directories : dxmt_include_path,
  c_args              : ['-ObjC'],
  link_args           : winemetal_unix_ld_args,
  native              : true,
  name_suffix         : 'so',
  install_rpath       : '',
  build_rpath         : '@loader_path/:@loader_path/../../',
  override_options    : { 'b_asneeded' : false },
)

meson.add_install_script(
  'install.sh', get_option('strip').to_string(), unix_install_dir, 
  install_tag: 'runtime'
)