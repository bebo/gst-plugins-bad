npp_sources = [
  'plugin.c',
  'gstnpploader.c',
  'gstnppscale.c',
]

npp_dep = dependency('', required : false)
nppig_dep = dependency('', required : false)
nppicc_dep = dependency('', required : false)

npp_option = get_option('npp')

if npp_option.disabled()
  subdir_done()
endif

if not gstcuda_dep.found()
  if npp_option.enabled()
    error('The cuda plugin was enabled explicitly, but required CUDA library was not found.')
  endif

  subdir_done()
endif

if host_machine.system() == 'windows'
  # On windows, CUDA_PATH env will be set by installer
  cuda_root = run_command(python3, '-c', 'import os; print(os.environ.get("CUDA_PATH"))').stdout().strip()
  if cuda_root != '' and cuda_root != 'None'
    arc = ''
    if build_machine.cpu_family() == 'x86_64'
      arc = 'x64'
    else
      arc = 'Win32'
    endif

    npp_libdir = join_paths (cuda_root, 'lib', arc)
    npp_incdir = join_paths (cuda_root, 'include')
    nppig_lib = cc.find_library('nppig', dirs: npp_libdir, required: false)
    nppicc_lib = cc.find_library('nppicc', dirs: npp_libdir, required: false)
    if nppig_lib.found() and nppicc_lib.found() and cc.has_header('npp.h', args: '-I' + npp_incdir)
      npp_dep = declare_dependency(include_directories: include_directories(cuda_incdir),
                                   dependencies: [nppig_lib, nppicc_lib])
    endif
  endif
else
  cuda_versions = [
    '10.1',
    '10.0',
    '9.2',
    '9.1',
    '9.0',
    '8.0',
    '7.5',
    '7.0',
    '6.5',
  ]
  cuda_ver = ''

  # FIXME: use break syntax when we use meson >= '0.49'
  foreach v : cuda_versions
    if cuda_ver == ''
      nppig_dep = dependency('nppig-' + v, required: false)
      nppicc_dep = dependency('nppicc-' + v, required: false)
      if nppig_dep.found() and nppicc_dep.found()
        cuda_ver = v
      endif
    endif
  endforeach

  if nppig_dep.found() and nppicc_dep.found()
    if cc.has_header('npp.h', dependencies: nppig_dep)
      npp_dep = nppig_dep.partial_dependency(compile_args : true, includes : true)
    endif
  endif
endif

if npp_dep.found ()
  gstnpp = library('gstnpp',
    npp_sources,
    c_args : gst_plugins_bad_args,
    include_directories : [configinc],
    dependencies : [gstbase_dep, gstvideo_dep, gstpbutils_dep, gstcuda_dep, npp_dep],
    install : true,
    install_dir : plugins_install_dir,
  )
  pkgconfig.generate(gstnpp, install_dir : plugins_pkgconfig_install_dir)
endif
