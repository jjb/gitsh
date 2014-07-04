#include "ruby.h"

VALUE gitsh = Qnil;
VALUE line_editor = Qnil;

void
Init_line_editor()
{
    gitsh = rb_define_module("Gitsh");
    line_editor = rb_define_module_under(gitsh, "LineEditor");
}
