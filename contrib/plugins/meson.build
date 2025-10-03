contrib_plugins = ['bbv', 'cache', 'cflow', 'drcov', 'execlog', 'hotblocks',
                   'hotpages', 'howvec', 'hwprofile', 'ips', 'stoptrigger']
if host_os != 'windows'
  # lockstep uses socket.h
  contrib_plugins += 'lockstep'
  # opcode uses UNIX sockets, glib and capstone (x86-64 only)
  contrib_plugins += 'opcode'
  capstone_dep = dependency('capstone', required: true)
endif

t = []
if get_option('plugins')
  foreach i : contrib_plugins
    if host_os == 'windows'
      t += shared_module(i, files(i + '.c') + 'win32_linker.c',
                        include_directories: '../../include/qemu',
                        link_depends: [win32_qemu_plugin_api_lib],
                        link_args: win32_qemu_plugin_api_link_flags,
                        dependencies: glib)
    else
      if i == 'opcode'
        t += shared_module(i, files(i + '.c'),
                          include_directories: '../../include/qemu',
                          dependencies: [glib, capstone_dep])      
      else
        t += shared_module(i, files(i + '.c'),
                          include_directories: '../../include/qemu',
                          dependencies: glib)
      endif
    endif
  endforeach
endif

if t.length() > 0
  alias_target('contrib-plugins', t)
else
  run_target('contrib-plugins', command: [python, '-c', ''])
endif

plugin_modules += t
