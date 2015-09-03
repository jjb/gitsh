#include "ruby.h"
#include "ruby/io.h"
#include "ruby/thread.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <readline/readline.h>
#include <readline/history.h>

#define OutputStringValue(str) do {\
    SafeStringValue(str);\
    (str) = rb_str_conv_enc((str), rb_enc_get(str), rb_locale_encoding());\
} while (0)\

VALUE m_readline = Qnil;
VALUE gitsh = Qnil;
VALUE line_editor = Qnil;

static VALUE hist_to_s(VALUE);
static VALUE hist_get(VALUE, VALUE);
static VALUE hist_set(VALUE, VALUE, VALUE);
static VALUE hist_push(VALUE, VALUE);
static VALUE hist_push_method(int, VALUE*, VALUE);
static VALUE hist_pop(VALUE);
static VALUE hist_shift(VALUE);
static VALUE hist_each(VALUE);
static VALUE hist_length(VALUE);
static VALUE hist_length(VALUE);
static VALUE hist_empty_p(VALUE);
static VALUE hist_delete_at(VALUE, VALUE);
static VALUE hist_clear(VALUE);

VALUE line_editor_history(VALUE);
VALUE line_editor_set_completion_append_character(VALUE, VALUE);
VALUE line_editor_set_completion_proc(VALUE, VALUE);
VALUE line_editor_set_input(VALUE, VALUE);
VALUE line_editor_set_output(VALUE, VALUE);
VALUE line_editor_readline(VALUE, VALUE, VALUE);
VALUE line_editor_line_buffer(VALUE);
VALUE line_editor_emacs_editing_mode(VALUE);

static VALUE readline_instream;
static VALUE history;

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
    using_history();

    rb_require("readline");
    m_readline = rb_const_get(rb_cObject, rb_intern("Readline"));

    history = rb_obj_alloc(rb_cObject);
    rb_extend_object(history, rb_mEnumerable);
    rb_define_singleton_method(history,"to_s", hist_to_s, 0);
    rb_define_singleton_method(history,"[]", hist_get, 1);
    rb_define_singleton_method(history,"[]=", hist_set, 2);
    rb_define_singleton_method(history,"<<", hist_push, 1);
    rb_define_singleton_method(history,"push", hist_push_method, -1);
    rb_define_singleton_method(history,"pop", hist_pop, 0);
    rb_define_singleton_method(history,"shift", hist_shift, 0);
    rb_define_singleton_method(history,"each", hist_each, 0);
    rb_define_singleton_method(history,"length", hist_length, 0);
    rb_define_singleton_method(history,"size", hist_length, 0);
    rb_define_singleton_method(history,"empty?", hist_empty_p, 0);
    rb_define_singleton_method(history,"delete_at", hist_delete_at, 1);
    rb_define_singleton_method(history,"clear", hist_clear, 0);

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

    rb_gc_register_address(&readline_instream);
}

static VALUE
hist_to_s(VALUE self)
{
    return rb_str_new_cstr("history");
}

static VALUE
hist_get(VALUE self, VALUE index)
{
    HIST_ENTRY *entry = NULL;
    int i;
   
    i = NUM2INT(index);
    if (i < 0) {
        i += history_length;
    }
    if (i >= 0) {
        entry = history_get(i);
    }
    if (entry == NULL) {
        rb_raise(rb_eIndexError, "invalid index");
    }
    return rb_locale_str_new_cstr(entry->line);
}

static VALUE
hist_set(VALUE self, VALUE index, VALUE str)
{
    HIST_ENTRY *entry = NULL;
    int i;

    i = NUM2INT(index);
    OutputStringValue(str);
    if (i < 0) {
        i += history_length;
    }
    if (i >= 0) {
        entry = replace_history_entry(i, RSTRING_PTR(str), NULL);
    }
    if (entry == NULL) {
        rb_raise(rb_eIndexError, "invalid index");
    }
    return str;
}

static VALUE
hist_push(VALUE self, VALUE str)
{
    OutputStringValue(str);
    add_history(RSTRING_PTR(str));
    return self;
}

static VALUE
hist_push_method(int argc, VALUE *argv, VALUE self)
{
    VALUE str;

    while (argc--) {
        str = *argv++;
        OutputStringValue(str);
        add_history(RSTRING_PTR(str));
    }
    return self;
}

static VALUE
rb_remove_history(int index)
{
    HIST_ENTRY *entry;
    VALUE val;

    entry = remove_history(index);
    if (entry) {
        val = rb_locale_str_new_cstr(entry->line);
        free((void *) entry->line);
        free(entry);
        return val;
    }
    return Qnil;
}

static VALUE
hist_pop(VALUE self)
{
    if (history_length > 0) {
        return rb_remove_history(history_length - 1);
    } else {
        return Qnil;
    }
}

static VALUE
hist_shift(VALUE self)
{
    if (history_length > 0) {
        return rb_remove_history(0);
    } else {
        return Qnil;
    }
}

static VALUE
hist_each(VALUE self)
{
    HIST_ENTRY *entry;
    int i;

    RETURN_ENUMERATOR(self, 0, 0);

    for (i = 0; i < history_length; i++) {
        entry = history_get(i);
        if (entry == NULL)
            break;
        rb_yield(rb_locale_str_new_cstr(entry->line));
    }
    return self;
}

static VALUE
hist_length(VALUE self)
{
    return INT2NUM(history_length);
}

static VALUE
hist_empty_p(VALUE self)
{
    return history_length == 0 ? Qtrue : Qfalse;
}

static VALUE
hist_delete_at(VALUE self, VALUE index)
{
    int i;

    i = NUM2INT(index);
    if (i < 0)
        i += history_length;
    if (i < 0 || i > history_length - 1) {
        rb_raise(rb_eIndexError, "invalid index");
    }
    return rb_remove_history(i);
}

static VALUE
hist_clear(VALUE self)
{
    clear_history();
    return self;
}

VALUE
line_editor_history(VALUE module)
{
    return history;
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

    readline_instream = input;
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

    return (VALUE)input;
}

VALUE
line_editor_readline(VALUE module, VALUE prompt, VALUE add_to_history)
{
    char *input;
    int exception;

    if (readline_instream) {
        rb_io_t *ifp;
        rb_io_check_initialized(ifp = RFILE(rb_io_taint_check(readline_instream))->fptr);
        //if (ifp->fd < 0) {
            //clear_rl_instream();
            //rb_raise(rb_eIOError, "closed readline input");
        //}
    }

    rb_str_locktmp(prompt);
    input = (char*)rb_protect(readline_get, prompt, &exception);
    rb_str_unlocktmp(prompt);
    if (exception) {
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
