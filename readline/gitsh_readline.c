#include "ruby.h"

VALUE GitshReadline = Qnil;

void
Init_gitsh_readline()
{
    GitshReadline = rb_define_module("GitshReadline");
}
