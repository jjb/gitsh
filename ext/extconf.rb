require 'mkmf'

extension_name = 'gitsh_readline'

have_library('readline', 'readline')

dir_config(extension_name)
create_makefile(extension_name)
