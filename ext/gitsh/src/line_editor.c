#include "ruby.h"
#include "ruby/io.h"
#include "ruby/thread.h"

#include <stdio.h>
#include <errno.h>

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

static int
readline_event()
{
    rb_wait_for_single_fd(fileno(rl_instream), RB_WAITFD_IN, NULL);
    return 0;
}

void
Init_line_editor()
{
    // Set the application name to gitsh. This allows for conditionals in
    // the inputrc file. For example:
    //
    // FIXME: Add this to the test suite
    // 
    //     $if gitsh
    //     # Specific settings here
    //     $endif
    rl_readline_name = (char*)"gitsh";

    rl_event_hook = readline_event;
    rl_initialize();

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
    rb_io_t *fp;
    int fd;
    FILE *f;

    Check_Type(input, T_FILE);
    GetOpenFile(input, fp);
    fd = rb_cloexec_dup(fp->fd);
    if (fd == -1) {
        rb_sys_fail("dup");
    }
    f = fdopen(fd, "r");
    if (f == NULL) {
        int save_errno = errno;
        close(fd);
        errno = save_errno;
        rb_sys_fail("fdopen");
    }
    rl_instream = f;

    return input;
}

VALUE
line_editor_set_output(VALUE module, VALUE output)
{
    rb_io_t *fp;
    int fd;
    FILE *f;

    Check_Type(output, T_FILE);
    GetOpenFile(output, fp);
    fd = rb_cloexec_dup(fp->fd);
    if (fd == -1) {
        rb_sys_fail("dup");
    }
    f = fdopen(fd, "w");
    if (f == NULL) {
        int save_errno = errno;
        close(fd);
        errno = save_errno;
        rb_sys_fail("fdopen");
    }
    rl_outstream = f;

    return output;
}

VALUE
readline_get(VALUE prompt)
{
    char *input = readline(StringValuePtr(prompt));
    printf("got: %s\n", input);

    return (VALUE)input;
}

VALUE
line_editor_readline(VALUE module, VALUE prompt, VALUE add_to_history)
{
    char *input;
    int exception;

    rb_str_locktmp(prompt);
    input = (char*)rb_protect(readline_get, prompt, &exception);
    rb_str_unlocktmp(prompt);
    if (exception) {
        printf("exception: %d\n", exception);
        rl_free_line_state();
        rl_cleanup_after_signal();
        rl_deprep_terminal();
        rb_jump_tag(exception);
    }

    if (input == NULL) {
        return Qnil;
    } else {
        return rb_str_new_cstr(input);
    }
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
