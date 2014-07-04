require 'mkmf'

have_library('readline', 'readline')

create_makefile('gitsh/line_editor', 'gitsh')
