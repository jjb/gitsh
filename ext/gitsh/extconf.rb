require 'mkmf'

have_header('readline/readline.h')
have_header('readline/history.h')
have_library('readline', 'readline')

create_makefile('gitsh/line_editor', 'src')
