#include "ruby.h"

#include <readline/readline.h>

VALUE m_readline = Qnil;
VALUE gitsh = Qnil;
VALUE line_editor = Qnil;

VALUE line_editor_history(VALUE);
VALUE line_editor_set_completion_append_character(VALUE, VALUE);
VALUE line_editor_set_completion_proc(VALUE, VALUE);
VALUE line_editor_set_input(VALUE, VALUE);
VALUE line_editor_set_output(VALUE, VALUE);
VALUE line_editor_readline(VALUE, VALUE, VALUE);
VALUE line_editor_line_buffer(VALUE);
VALUE line_editor_emacs_editing_mode(VALUE);

void
Init_line_editor()
{
    rb_require("readline");
    m_readline = rb_const_get(rb_cObject, rb_intern("Readline"));

    gitsh = rb_define_module("Gitsh");
    line_editor = rb_define_module_under(gitsh, "LineEditor");

    rb_define_singleton_method(line_editor, "history",
        line_editor_history, 0);
    rb_define_singleton_method(line_editor, "completion_append_character=",
        line_editor_set_completion_append_character, 1);
    rb_define_singleton_method(line_editor, "completion_proc=",
        line_editor_set_completion_proc, 1);
    rb_define_singleton_method(line_editor, "input=",
        line_editor_set_input, 1);
    rb_define_singleton_method(line_editor, "output=",
        line_editor_set_output, 1);
    rb_define_singleton_method(line_editor, "readline",
        line_editor_readline, 2);
    rb_define_singleton_method(line_editor, "line_buffer",
        line_editor_line_buffer, 0);
    rb_define_singleton_method(line_editor, "emacs_editing_mode",
        line_editor_emacs_editing_mode, 0);
}

VALUE
line_editor_history(VALUE module)
{
    return rb_const_get(m_readline, rb_intern("HISTORY"));
}

VALUE
line_editor_set_completion_append_character(VALUE module, VALUE append_chr)
{
    return rb_funcall(m_readline, rb_intern("completion_append_character="), 1,
        append_chr);
}

VALUE
line_editor_set_completion_proc(VALUE module, VALUE proc)
{
    return rb_funcall(m_readline, rb_intern("completion_proc="), 1, proc);
}

VALUE
line_editor_set_input(VALUE module, VALUE input)
{
    return rb_funcall(m_readline, rb_intern("input="), 1, input);
}

VALUE
line_editor_set_output(VALUE module, VALUE output)
{
    return rb_funcall(m_readline, rb_intern("output="), 1, output);
}

VALUE
line_editor_readline(VALUE module, VALUE prompt, VALUE add_to_history)
{
    return rb_funcall(m_readline, rb_intern("readline"), 2,
        prompt, add_to_history);
}

VALUE
line_editor_line_buffer(VALUE module)
{
    return rb_funcall(m_readline, rb_intern("line_buffer"), 0);
}

VALUE
line_editor_emacs_editing_mode(VALUE module)
{
    return rb_funcall(m_readline, rb_intern("emacs_editing_mode"), 0);
}
