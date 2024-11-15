/************************************************

  Based on Ruby 2.3.0's
  readline.c - GNU Readline module

  $Author$
  created at: Wed Jan 20 13:59:32 JST 1999

  Copyright (C) 1997-2008  Shugo Maeda
  Copyright (C) 2008-2013  Kouji Takao

  $Id$

  Contact:
   - Kouji Takao <kouji dot takao at gmail dot com> (current maintainer)

************************************************/

#ifdef RUBY_EXTCONF_H
#include RUBY_EXTCONF_H
#endif

#include "ruby/config.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "ruby.h"
#include "ruby/io.h"
#include "ruby/thread.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#if !defined(RARRAY_AREF)
#define RARRAY_AREF(a, i) RARRAY_PTR(a)[i]
#endif

static VALUE mGitsh;
static VALUE mLineEditor;

#define EDIT_LINE_LIBRARY_VERSION "EditLine wrapper"
#ifndef USE_INSERT_IGNORE_ESCAPE
# if !defined(HAVE_EDITLINE_READLINE_H) && defined(RL_PROMPT_START_IGNORE) && defined(RL_PROMPT_END_IGNORE)
#  define USE_INSERT_IGNORE_ESCAPE 1
# else
#  define USE_INSERT_IGNORE_ESCAPE 0
# endif
#endif

#define COMPLETION_PROC "completion_proc"
#define COMPLETION_CASE_FOLD "completion_case_fold"
#define QUOTING_DETECTION_PROC "quoting_detection_proc"
static ID completion_proc, completion_case_fold, quoting_detection_proc;
#if USE_INSERT_IGNORE_ESCAPE
static ID id_orig_prompt, id_last_prompt;
#endif
#if defined(HAVE_RL_PRE_INPUT_HOOK)
static ID id_pre_input_hook;
#endif
#if defined(HAVE_RL_SPECIAL_PREFIXES)
static ID id_special_prefixes;
#endif

#ifndef HAVE_RL_FILENAME_COMPLETION_FUNCTION
# define rl_filename_completion_function filename_completion_function
#endif
#ifndef HAVE_RL_USERNAME_COMPLETION_FUNCTION
# define rl_username_completion_function username_completion_function
#endif
#ifndef HAVE_RL_COMPLETION_MATCHES
# define rl_completion_matches completion_matches
#endif

static int (*history_get_offset_func)(int);
static int (*history_replace_offset_func)(int);
static int readline_completion_append_character;

static char **readline_attempted_completion_function(const char *text,
                                                     int start, int end);
int readline_char_is_quoted(char *text, int index);
long byte_index_to_char_index(VALUE str, long byte_index);

#define OutputStringValue(str) do {\
    SafeStringValue(str);\
    (str) = rb_str_conv_enc((str), rb_enc_get(str), rb_locale_encoding());\
} while (0)\

#if !defined(HAVE_RB_OBJ_HIDE)
#define rb_obj_hide(str) do { RBASIC(str)->klass = 0; } while (0);
#endif

#if !defined(HAVE_RB_OBJ_REVEAL)
#define rb_obj_reveal(str, newKlass) do {\
  RBASIC(str)->klass = newKlass;\
} while (0);
#endif


/*
 * Document-class: Gitsh::LineEditor
 *
 * The Gitsh::LineEditor module provides interface for GNU Readline.
 * It is based on Ruby's Readline module, but provides slightly different
 * features.
 *
 * GNU Readline:: http://www.gnu.org/directory/readline.html
 *
 * The Gitsh::LineEditor.readline method reads one line of user input,
 * allowing the user to take advantage of GNU Readline's Emacs- or vi-like
 * key bindings for line editing.
 *
 *   require "gitsh/line_editor"
 *   while buf = Gitsh::LineEditor.readline("> ", true)
 *     p buf
 *   end
 *
 * The user's input can be recorded to the history.
 * The history can be accessed via Gitsh::LineEditor::HISTORY constant.
 *
 *   require "gitsh/line_editor"
 *   while buf = Gitsh::LineEditor.readline("> ", true)
 *     p Gitsh::LineEditor::HISTORY.to_a
 *     print("-> ", buf, "\n")
 *   end
 *
 */

static VALUE readline_instream;
static VALUE readline_outstream;
static FILE *readline_rl_instream;
static FILE *readline_rl_outstream;

#if defined HAVE_RL_GETC_FUNCTION

#ifndef HAVE_RL_GETC
#define rl_getc(f) EOF
#endif

struct getc_struct {
  FILE *input;
  int fd;
  int ret;
  int err;
};

static int
getc_body(struct getc_struct *p)
{
    char ch;
    ssize_t ss;

#if defined(_WIN32)
    {
        INPUT_RECORD ir;
        int n;
        static int prior_key = '0';
        for (;;) {
            if (prior_key > 0xff) {
                prior_key = rl_getc(p->input);
                return prior_key;
            }
            if (PeekConsoleInput((HANDLE)_get_osfhandle(p->fd), &ir, 1, &n)) {
                if (n == 1) {
                    if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
                        prior_key = rl_getc(p->input);
                        return prior_key;
                    } else {
                        ReadConsoleInput((HANDLE)_get_osfhandle(p->fd), &ir, 1, &n);
                    }
                } else {
                    HANDLE h = (HANDLE)_get_osfhandle(p->fd);
                    rb_w32_wait_events(&h, 1, INFINITE);
                }
            } else {
                break;
            }
        }
    }
#endif

    ss = read(p->fd, &ch, 1);
    if (ss == 0) {
        errno = 0;
        return EOF;
    }
    if (ss != 1)
        return EOF;
    return (unsigned char)ch;
}

static void *
getc_func(void *data1)
{
    struct getc_struct *p = data1;
    errno = 0;
    p->ret = getc_body(p);
    p->err = errno;
    return NULL;
}

static int
readline_getc(FILE *input)
{
    struct getc_struct data;
    if (input == NULL) /* editline may give NULL as input. */
        input = stdin;
    data.input = input;
    data.fd = fileno(input);
  again:
    data.ret = EOF;
    data.err = EINTR; /* getc_func is not called if already interrupted. */
    rb_thread_call_without_gvl2(getc_func, &data, RUBY_UBF_IO, NULL);
    if (data.ret == EOF) {
        if (data.err == 0) {
            return EOF;
        }
        if (data.err == EINTR) {
            rb_thread_check_ints();
            goto again;
        }
        if (data.err == EWOULDBLOCK || data.err == EAGAIN) {
            int ret;
            if (fileno(input) != data.fd)
                rb_bug("readline_getc: input closed unexpectedly or memory corrupted");
            ret = rb_wait_for_single_fd(data.fd, RB_WAITFD_IN, NULL);
            if (ret != -1 || errno == EINTR)
                goto again;
            rb_sys_fail("rb_wait_for_single_fd");
        }
        rb_syserr_fail(data.err, "read");
    }
    return data.ret;
}

#elif defined HAVE_RL_EVENT_HOOK
#define BUSY_WAIT 0

static int readline_event(void);
static int
readline_event(void)
{
#if BUSY_WAIT
    rb_thread_schedule();
#else
    rb_wait_for_single_fd(fileno(rl_instream), RB_WAITFD_IN, NULL);
    return 0;
#endif
}
#endif

int
readline_char_is_quoted(char *text, int byte_index)
{
    VALUE proc, result, str;
    long char_index;

    proc = rb_attr_get(mLineEditor, quoting_detection_proc);
    if (NIL_P(proc)) {
        return 0;
    }

    str = rb_locale_str_new_cstr(text);
    char_index = byte_index_to_char_index(str, (long)byte_index);

    if (char_index == -1) {
        rb_raise(rb_eIndexError, "failed to find character at byte index");
    }

    result = rb_funcall(proc, rb_intern("call"), 2, str, LONG2FIX(char_index));
    return result ? 1 : 0;
}

long
byte_index_to_char_index(VALUE str, long byte_index)
{
    const char *ptr;
    long ci, bi, len, clen;
    rb_encoding *enc;

    enc = rb_enc_get(str);
    len = RSTRING_LEN(str);
    ptr = RSTRING_PTR(str);

    for (bi = 0, ci = 0; bi < len; bi += clen, ++ci) {
        if (bi == byte_index) {
            return ci;
        }
        clen = rb_enc_mbclen(ptr + bi, ptr + len, enc);
    }

    return -1;
}

#if USE_INSERT_IGNORE_ESCAPE
static VALUE
insert_ignore_escape(VALUE self, VALUE prompt)
{
    VALUE last_prompt, orig_prompt = rb_attr_get(self, id_orig_prompt);
    int ignoring = 0;
    const char *s0, *s, *e;
    long len;
    static const char ignore_code[2] = {RL_PROMPT_START_IGNORE, RL_PROMPT_END_IGNORE};

    prompt = rb_str_new_shared(prompt);
    last_prompt = rb_attr_get(self, id_last_prompt);
    if (orig_prompt == prompt) return last_prompt;
    len = RSTRING_LEN(prompt);
    if (NIL_P(last_prompt)) {
        last_prompt = rb_str_tmp_new(len);
    }

    s = s0 = RSTRING_PTR(prompt);
    e = s0 + len;
    rb_str_set_len(last_prompt, 0);
    while (s < e && *s) {
        switch (*s) {
          case RL_PROMPT_START_IGNORE:
            ignoring = -1;
            rb_str_cat(last_prompt, s0, ++s - s0);
            s0 = s;
            break;
          case RL_PROMPT_END_IGNORE:
            ignoring = 0;
            rb_str_cat(last_prompt, s0, ++s - s0);
            s0 = s;
            break;
          case '\033':
            if (++s < e && *s == '[') {
                rb_str_cat(last_prompt, s0, s - s0 - 1);
                s0 = s - 1;
                while (++s < e && *s) {
                    if (ISALPHA(*(unsigned char *)s)) {
                        if (!ignoring) {
                            ignoring = 1;
                            rb_str_cat(last_prompt, ignore_code+0, 1);
                        }
                        rb_str_cat(last_prompt, s0, ++s - s0);
                        s0 = s;
                        break;
                    }
                    else if (!(('0' <= *s && *s <= '9') || *s == ';')) {
                        break;
                    }
                }
            }
            break;
          default:
            if (ignoring > 0) {
                ignoring = 0;
                rb_str_cat(last_prompt, ignore_code+1, 1);
            }
            s++;
            break;
        }
    }
    if (ignoring > 0) {
        ignoring = 0;
        rb_str_cat(last_prompt, ignore_code+1, 1);
    }
    rb_str_cat(last_prompt, s0, s - s0);

    rb_ivar_set(self, id_orig_prompt, prompt);
    rb_ivar_set(self, id_last_prompt, last_prompt);

    return last_prompt;
}
#endif

static VALUE
readline_get(VALUE prompt)
{
    readline_completion_append_character = rl_completion_append_character;
    return (VALUE)readline((char *)prompt);
}

static void
clear_rl_instream(void)
{
    if (readline_rl_instream) {
        fclose(readline_rl_instream);
        if (rl_instream == readline_rl_instream)
            rl_instream = NULL;
        readline_rl_instream = NULL;
    }
    readline_instream = Qfalse;
}

static void
clear_rl_outstream(void)
{
    if (readline_rl_outstream) {
        fclose(readline_rl_outstream);
        if (rl_outstream == readline_rl_outstream)
            rl_outstream = NULL;
        readline_rl_outstream = NULL;
    }
    readline_outstream = Qfalse;
}

static void
prepare_readline(void)
{
    static int initialized = 0;
    if (!initialized) {
  rl_initialize();
  initialized = 1;
    }

    if (readline_instream) {
        rb_io_t *ifp;
        rb_io_check_initialized(ifp = RFILE(rb_io_taint_check(readline_instream))->fptr);
        if (ifp->fd < 0) {
            clear_rl_instream();
            rb_raise(rb_eIOError, "closed readline input");
        }
    }

    if (readline_outstream) {
        rb_io_t *ofp;
        rb_io_check_initialized(ofp = RFILE(rb_io_taint_check(readline_outstream))->fptr);
        if (ofp->fd < 0) {
            clear_rl_outstream();
            rb_raise(rb_eIOError, "closed readline output");
        }
    }
}

/*
 * call-seq:
 *   Gitsh::LineEditor.readline(prompt = "", add_hist = false) -> string or nil
 *
 * Shows the +prompt+ and reads the inputted line with line editing.
 * The inputted line is added to the history if +add_hist+ is true.
 * History is accessible via the +Gitsh::LineEditor::HISTORY+ constant.
 *
 * Returns nil when the inputted line is empty and user inputs EOF
 * (Presses ^D on UNIX).
 *
 * Raises +Interrupt+ when the user presses ^C instead of entering a line.
 * This exception will need to be caught, or interrupt signals trapped, if
 * you don't want ^C to terminate the ruby interpreter.
 *
 * Raises IOError exception if one of below conditions are satisfied.
 * 1. stdin was closed.
 * 2. stdout was closed.
 *
 * This method supports thread. Switches the thread context when waits
 * inputting line.
 *
 * Supports line edit when inputs line. Provides VI and Emacs editing mode.
 * Default is Emacs editing mode.
 */
static VALUE
readline_readline(int argc, VALUE *argv, VALUE self)
{
    VALUE tmp, add_hist, result;
    char *prompt = NULL;
    char *buff;
    int status;

    if (rb_scan_args(argc, argv, "02", &tmp, &add_hist) > 0) {
        OutputStringValue(tmp);
#if USE_INSERT_IGNORE_ESCAPE
        tmp = insert_ignore_escape(self, tmp);
        rb_str_locktmp(tmp);
#endif
        prompt = RSTRING_PTR(tmp);
    }

    prepare_readline();

#ifdef _WIN32
    rl_prep_terminal(1);
#endif
    buff = (char*)rb_protect(readline_get, (VALUE)prompt, &status);
#if USE_INSERT_IGNORE_ESCAPE
    if (prompt) {
        rb_str_unlocktmp(tmp);
    }
#endif
    if (status) {
#if defined HAVE_RL_CLEANUP_AFTER_SIGNAL
        /* restore terminal mode and signal handler*/
#if defined HAVE_RL_FREE_LINE_STATE
        rl_free_line_state();
#endif
        rl_cleanup_after_signal();
#elif defined HAVE_RL_DEPREP_TERM_FUNCTION
        /* restore terminal mode */
        if (rl_deprep_term_function != NULL) /* NULL in libedit. [ruby-dev:29116] */
            (*rl_deprep_term_function)();
        else
#else
        rl_deprep_terminal();
#endif
        rb_jump_tag(status);
    }

    if (RTEST(add_hist) && buff) {
        add_history(buff);
    }
    if (buff) {
        result = rb_locale_str_new_cstr(buff);
    }
    else
        result = Qnil;
    if (buff) free(buff);
    return result;
}

/*
 * call-seq:
 *   Gitsh::LineEditor.input = input
 *
 * Specifies a File object +input+ that is input stream for
 * Gitsh::LineEditor.readline method.
 */
static VALUE
readline_s_set_input(VALUE self, VALUE input)
{
    rb_io_t *ifp;
    int fd;
    FILE *f;

    if (NIL_P(input)) {
        clear_rl_instream();
    }
    else {
        Check_Type(input, T_FILE);
        GetOpenFile(input, ifp);
        clear_rl_instream();
        fd = rb_cloexec_dup(ifp->fd);
        if (fd == -1)
            rb_sys_fail("dup");
        f = fdopen(fd, "r");
        if (f == NULL) {
            int save_errno = errno;
            close(fd);
            rb_syserr_fail(save_errno, "fdopen");
        }
        rl_instream = readline_rl_instream = f;
        readline_instream = input;
    }
    return input;
}

/*
 * call-seq:
 *   Gitsh::LineEditor.output = output
 *
 * Specifies a File object +output+ that is output stream for
 * Gitsh::LineEditor.readline method.
 */
static VALUE
readline_s_set_output(VALUE self, VALUE output)
{
    rb_io_t *ofp;
    int fd;
    FILE *f;

    if (NIL_P(output)) {
        clear_rl_outstream();
    }
    else {
        Check_Type(output, T_FILE);
        GetOpenFile(output, ofp);
        clear_rl_outstream();
        fd = rb_cloexec_dup(ofp->fd);
        if (fd == -1)
            rb_sys_fail("dup");
        f = fdopen(fd, "w");
        if (f == NULL) {
            int save_errno = errno;
            close(fd);
            rb_syserr_fail(save_errno, "fdopen");
        }
        rl_outstream = readline_rl_outstream = f;
        readline_outstream = output;
    }
    return output;
}

#if defined(HAVE_RL_PRE_INPUT_HOOK)
/*
 * call-seq:
 *   Gitsh::LineEditor.pre_input_hook = proc
 *
 * Specifies a Proc object +proc+ to call after the first prompt has
 * been printed and just before readline starts reading input
 * characters.
 *
 * See GNU Readline's rl_pre_input_hook variable.
 *
 * Raises ArgumentError if +proc+ does not respond to the call method.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_set_pre_input_hook(VALUE self, VALUE proc)
{
    if (!NIL_P(proc) && !rb_respond_to(proc, rb_intern("call")))
        rb_raise(rb_eArgError, "argument must respond to `call'");
    return rb_ivar_set(mLineEditor, id_pre_input_hook, proc);
}

/*
 * call-seq:
 *   Gitsh::LineEditor.pre_input_hook -> proc
 *
 * Returns a Proc object +proc+ to call after the first prompt has
 * been printed and just before readline starts reading input
 * characters. The default is nil.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_get_pre_input_hook(VALUE self)
{
    return rb_attr_get(mLineEditor, id_pre_input_hook);
}

static int
readline_pre_input_hook(void)
{
    VALUE proc;

    proc = rb_attr_get(mLineEditor, id_pre_input_hook);
    if (!NIL_P(proc))
        rb_funcall(proc, rb_intern("call"), 0);
    return 0;
}
#else
#define readline_s_set_pre_input_hook rb_f_notimplement
#define readline_s_get_pre_input_hook rb_f_notimplement
#endif

#if defined(HAVE_RL_INSERT_TEXT)
/*
 * call-seq:
 *   Gitsh::LineEditor.insert_text(string) -> self
 *
 * Insert text into the line at the current cursor position.
 *
 * See GNU Readline's rl_insert_text function.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_insert_text(VALUE self, VALUE str)
{
    OutputStringValue(str);
    rl_insert_text(RSTRING_PTR(str));
    return self;
}
#else
#define readline_s_insert_text rb_f_notimplement
#endif

#if defined(HAVE_RL_DELETE_TEXT)
static VALUE
readline_s_delete_bytes(VALUE self, VALUE r_beg, VALUE r_len)
{
    int beg = NUM2INT(r_beg), len = NUM2INT(r_len);
    rl_delete_text(beg, beg + len);
    return self;
}
#else
#define readline_s_delete_bytes rb_f_notimplement
#endif

#if defined(HAVE_RL_REDISPLAY)
/*
 * call-seq:
 *   Gitsh::LineEditor.redisplay -> self
 *
 * Change what's displayed on the screen to reflect the current
 * contents.
 *
 * See GNU Readline's rl_redisplay function.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_redisplay(VALUE self)
{
    rl_redisplay();
    return self;
}
#else
#define readline_s_redisplay rb_f_notimplement
#endif

/*
 * call-seq:
 *   Gitsh::LineEditor.completion_proc = proc
 *
 * Specifies a Proc object +proc+ to determine completion behavior.  It
 * should take input string and return an array of completion candidates.
 *
 * GNU Readline's default file path completion is used if +proc+ is nil.
 *
 * The String that is passed to the Proc depends on the
 * Gitsh::LineEditor.completer_word_break_characters property.
 * By default the word under the cursor is passed to the Proc. For example,
 * if the input is "foo bar" then only "bar" would be passed to the completion
 * Proc.
 *
 * Upon successful completion the Gitsh::LineEditor.completion_append_character
 * will be appended to the input so the user can start working on their next
 * argument.
 *
 * Raises ArgumentError if +proc+ does not respond to the call method.
 */
static VALUE
readline_s_set_completion_proc(VALUE self, VALUE proc)
{
    if (!NIL_P(proc) && !rb_respond_to(proc, rb_intern("call")))
        rb_raise(rb_eArgError, "argument must respond to `call'");
    return rb_ivar_set(mLineEditor, completion_proc, proc);
}

/*
 * call-seq:
 *   Gitsh::LineEditor.completion_proc -> proc
 *
 * Returns the completion Proc object.
 */
static VALUE
readline_s_get_completion_proc(VALUE self)
{
    return rb_attr_get(mLineEditor, completion_proc);
}

static VALUE
readline_s_set_quoting_detection_proc(VALUE self, VALUE proc)
{
    if (!NIL_P(proc) && !rb_respond_to(proc, rb_intern("call")))
        rb_raise(rb_eArgError, "argument must respond to `call'");
    return rb_ivar_set(mLineEditor, quoting_detection_proc, proc);
}

static VALUE
readline_s_get_quoting_detection_proc(VALUE self)
{
    return rb_attr_get(mLineEditor, quoting_detection_proc);
}

/*
 * call-seq:
 *   Gitsh::LineEditor.completion_case_fold = bool
 *
 * Sets whether or not to ignore case on completion.
 */
static VALUE
readline_s_set_completion_case_fold(VALUE self, VALUE val)
{
    return rb_ivar_set(mLineEditor, completion_case_fold, val);
}

/*
 * call-seq:
 *   Gitsh::LineEditor.completion_case_fold -> bool
 *
 * Returns true if completion ignores case. If no, returns false.
 *
 * NOTE: Returns the same object that is specified by
 * Gitsh::LineEditor.completion_case_fold= method.
 */
static VALUE
readline_s_get_completion_case_fold(VALUE self)
{
    return rb_attr_get(mLineEditor, completion_case_fold);
}

/*
 * call-seq:
 *   Gitsh::LineEditor.line_buffer -> string
 *
 * Returns the full line that is being edited. This is useful from
 * within the complete_proc for determining the context of the
 * completion request.
 *
 * The length of +Gitsh::LineEditor.line_buffer+ and GNU Readline's rl_end are
 * same.
 */
static VALUE
readline_s_get_line_buffer(VALUE self)
{
    if (rl_line_buffer == NULL)
        return Qnil;
    return rb_locale_str_new_cstr(rl_line_buffer);
}

#ifdef HAVE_RL_POINT
/*
 * call-seq:
 *   Gitsh::LineEditor.point -> int
 *
 * Returns the index of the current cursor position in
 * +Gitsh::LineEditor.line_buffer+.
 *
 * The index in +Gitsh::LineEditor.line_buffer+ which matches the start of
 * input-string passed to completion_proc is computed by subtracting
 * the length of input-string from +Gitsh::LineEditor.point+.
 *
 *   start = (the length of input-string) - Gitsh::LineEditor.point
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_get_point(VALUE self)
{
    return INT2NUM(rl_point);
}

/*
 * call-seq:
 *   Gitsh::LineEditor.point = int
 *
 * Set the index of the current cursor position in
 * +Gitsh::LineEditor.line_buffer+.
 *
 * Raises NotImplementedError if the using readline library does not support.
 *
 * See +Gitsh::LineEditor.point+.
 */
static VALUE
readline_s_set_point(VALUE self, VALUE pos)
{
    rl_point = NUM2INT(pos);
    return pos;
}
#else
#define readline_s_get_point rb_f_notimplement
#define readline_s_set_point rb_f_notimplement
#endif

static char **
readline_attempted_completion_function(const char *text, int start, int end)
{
    VALUE proc, ary, temp;
    char **result;
    int case_fold;
    long i, matches;
    rb_encoding *enc;
    VALUE encobj;

    proc = rb_attr_get(mLineEditor, completion_proc);
    if (NIL_P(proc))
        return NULL;
    rl_completion_append_character = readline_completion_append_character;
#ifdef HAVE_RL_ATTEMPTED_COMPLETION_OVER
    rl_attempted_completion_over = 1;
#endif
    case_fold = RTEST(rb_attr_get(mLineEditor, completion_case_fold));
    ary = rb_funcall(proc, rb_intern("call"), 1, rb_locale_str_new_cstr(text));
    if (!RB_TYPE_P(ary, T_ARRAY))
        ary = rb_Array(ary);
    matches = RARRAY_LEN(ary);
    if (matches == 0) return NULL;
    result = (char**)malloc((matches + 2)*sizeof(char*));
    if (result == NULL) rb_memerror();
    enc = rb_locale_encoding();
    encobj = rb_enc_from_encoding(enc);
    for (i = 0; i < matches; i++) {
        temp = rb_obj_as_string(RARRAY_AREF(ary, i));
        StringValueCStr(temp);  /* must be NUL-terminated */
        rb_enc_check(encobj, temp);
        result[i + 1] = (char*)malloc(RSTRING_LEN(temp) + 1);
        if (result[i + 1]  == NULL) rb_memerror();
        strcpy(result[i + 1], RSTRING_PTR(temp));
    }
    result[matches + 1] = NULL;

    if (matches == 1) {
        result[0] = strdup(result[1]);
    }
    else {
        const char *result1 = result[1];
        long low = strlen(result1);

        for (i = 1; i < matches; ++i) {
            register int c1, c2;
            long i1, i2, l2;
            int n1, n2;
            const char *p2 = result[i + 1];

            l2 = strlen(p2);
            for (i1 = i2 = 0; i1 < low && i2 < l2; i1 += n1, i2 += n2) {
                c1 = rb_enc_codepoint_len(result1 + i1, result1 + low, &n1, enc);
                c2 = rb_enc_codepoint_len(p2 + i2, p2 + l2, &n2, enc);
                if (case_fold) {
                    c1 = rb_tolower(c1);
                    c2 = rb_tolower(c2);
                }
                if (c1 != c2) break;
            }

            low = i1;
        }
        result[0] = (char*)malloc(low + 1);
        if (result[0]  == NULL) rb_memerror();
        strncpy(result[0], result[1], low);
        result[0][low] = '\0';
    }

    return result;
}

/*
 * call-seq:
 *   Gitsh::LineEditor.set_screen_size(rows, columns) -> self
 *
 * Set terminal size to +rows+ and +columns+.
 *
 * See GNU Readline's rl_set_screen_size function.
 */
static VALUE
readline_s_set_screen_size(VALUE self, VALUE rows, VALUE columns)
{
    rl_set_screen_size(NUM2INT(rows), NUM2INT(columns));
    return self;
}

#ifdef HAVE_RL_GET_SCREEN_SIZE
/*
 * call-seq:
 *   Gitsh::LineEditor.get_screen_size -> [rows, columns]
 *
 * Returns the terminal's rows and columns.
 *
 * See GNU Readline's rl_get_screen_size function.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_get_screen_size(VALUE self)
{
    int rows, columns;
    VALUE res;

    rl_get_screen_size(&rows, &columns);
    res = rb_ary_new();
    rb_ary_push(res, INT2NUM(rows));
    rb_ary_push(res, INT2NUM(columns));
    return res;
}
#else
#define readline_s_get_screen_size rb_f_notimplement
#endif

#ifdef HAVE_RL_VI_EDITING_MODE
/*
 * call-seq:
 *   Gitsh::LineEditor.vi_editing_mode -> nil
 *
 * Specifies VI editing mode. See the manual of GNU Readline for
 * details of VI editing mode.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_vi_editing_mode(VALUE self)
{
    rl_vi_editing_mode(1,0);
    return Qnil;
}
#else
#define readline_s_vi_editing_mode rb_f_notimplement
#endif

/*
 * call-seq:
 *   Gitsh::LineEditor.vi_editing_mode? -> bool
 *
 * Returns true if vi mode is active. Returns false if not.
 */
static VALUE
readline_s_vi_editing_mode_p(VALUE self)
{
    return rl_editing_mode == 0 ? Qtrue : Qfalse;
}

#ifdef HAVE_RL_EMACS_EDITING_MODE
/*
 * call-seq:
 *   Gitsh::LineEditor.emacs_editing_mode -> nil
 *
 * Specifies Emacs editing mode. The default is this mode. See the
 * manual of GNU Readline for details of Emacs editing mode.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_emacs_editing_mode(VALUE self)
{
    rl_emacs_editing_mode(1,0);
    return Qnil;
}
#else
#define readline_s_emacs_editing_mode rb_f_notimplement
#endif

/*
 * call-seq:
 *   Gitsh::LineEditor.emacs_editing_mode? -> bool
 *
 * Returns true if emacs mode is active. Returns false if not.
 */
static VALUE
readline_s_emacs_editing_mode_p(VALUE self)
{
    return rl_editing_mode == 1 ? Qtrue : Qfalse;
}

/*
 * call-seq:
 *   Gitsh::LineEditor.completion_append_character = char
 *
 * Specifies a character to be appended on completion.
 * Nothing will be appended if an empty string ("") or nil is
 * specified.
 *
 * NOTE: Only one character can be specified. When "string" is
 * specified, sets only "s" that is the first.
 */
static VALUE
readline_s_set_completion_append_character(VALUE self, VALUE str)
{
    if (NIL_P(str)) {
        rl_completion_append_character = '\0';
    }
    else {
        OutputStringValue(str);
        if (RSTRING_LEN(str) == 0) {
            rl_completion_append_character = '\0';
        } else {
            rl_completion_append_character = RSTRING_PTR(str)[0];
        }
    }
    return self;
}

/*
 * call-seq:
 *   Gitsh::LineEditor.completion_append_character -> char
 *
 * Returns a string containing a character to be appended on
 * completion. The default is a space (" ").
 */
static VALUE
readline_s_get_completion_append_character(VALUE self)
{
    char buf[1];

    if (rl_completion_append_character == '\0')
        return Qnil;

    buf[0] = (char) rl_completion_append_character;
    return rb_locale_str_new(buf, 1);
}

/*
 * call-seq:
 *   Gitsh::LineEditor.completion_suppress_quote = true_or_false
 *
 * When set to true, indicates that Readline should not append a closing
 * quote character when completing a quoted argument.
 *
 * Can only be set during a completion, i.e. from within your
 * LineEditor.completion_proc.
 */
static VALUE
readline_s_set_completion_supress_quote(VALUE self, VALUE flag)
{
    rl_completion_suppress_quote = RTEST(flag);
    return self;
}

/*
 * call-seq:
 *   Gitsh::LineEditor.completion_suppress_quote -> boolean
 *
 * Returns a boolean indicating if Readline will suppress the closing quote
 * when completion a quoted argument. The default is false (a closing
 * quote will be appended).
 */
static VALUE
readline_s_get_completion_supress_quote(VALUE self)
{
    return rl_completion_suppress_quote ? Qtrue : Qfalse;
}
/*
 * call-seq:
 *   Gitsh::LineEditor.completion_quote_character -> char
 *
 * When called during a completion (e.g. from within your completion_proc),
 * it will return a string containing the chracter used to quote the
 * argument being completed, or nil if the argument is unquoted.
 *
 * When called at other times, it will always return nil.
 */
static VALUE
readline_s_get_completion_quote_character(VALUE self)
{
    char buf[1];

    if (rl_completion_quote_character == '\0')
        return Qnil;

    buf[0] = (char) rl_completion_quote_character;
    return rb_locale_str_new(buf, 1);
}

#ifdef HAVE_RL_COMPLETER_WORD_BREAK_CHARACTERS
/*
 * call-seq:
 *   Gitsh::LineEditor.completer_word_break_characters = string
 *
 * Sets the basic list of characters that signal a break between words
 * for rl_complete_internal().
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_set_completer_word_break_characters(VALUE self, VALUE str)
{
    static char *completer_word_break_characters = NULL;

    OutputStringValue(str);
    if (completer_word_break_characters == NULL) {
        completer_word_break_characters =
            ALLOC_N(char, RSTRING_LEN(str) + 1);
    }
    else {
        REALLOC_N(completer_word_break_characters, char, RSTRING_LEN(str) + 1);
    }
    strncpy(completer_word_break_characters,
            RSTRING_PTR(str), RSTRING_LEN(str));
    completer_word_break_characters[RSTRING_LEN(str)] = '\0';
    rl_completer_word_break_characters = completer_word_break_characters;
    return self;
}
#else
#define readline_s_set_completer_word_break_characters rb_f_notimplement
#endif

#ifdef HAVE_RL_COMPLETER_WORD_BREAK_CHARACTERS
/*
 * call-seq:
 *   Gitsh::LineEditor.completer_word_break_characters -> string
 *
 * Gets the basic list of characters that signal a break between words
 * for rl_complete_internal().
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_get_completer_word_break_characters(VALUE str)
{
    if (rl_completer_word_break_characters == NULL)
        return Qnil;
    return rb_locale_str_new_cstr(rl_completer_word_break_characters);
}
#else
#define readline_s_get_completer_word_break_characters rb_f_notimplement
#endif

#if defined(HAVE_RL_SPECIAL_PREFIXES)
/*
 * call-seq:
 *   Gitsh::LineEditor.special_prefixes = string
 *
 * Sets the list of characters that are word break characters, but
 * should be left in text when it is passed to the completion
 * function. Programs can use this to help determine what kind of
 * completing to do. For instance, Bash sets this variable to "$@" so
 * that it can complete shell variables and hostnames.
 *
 * See GNU Readline's rl_special_prefixes variable.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_set_special_prefixes(VALUE self, VALUE str)
{
    if (!NIL_P(str)) {
        OutputStringValue(str);
        str = rb_str_dup_frozen(str);
        rb_obj_hide(str);
    }
    rb_ivar_set(mLineEditor, id_special_prefixes, str);
    if (NIL_P(str)) {
        rl_special_prefixes = NULL;
    }
    else {
        rl_special_prefixes = RSTRING_PTR(str);
    }
    return self;
}

/*
 * call-seq:
 *   Gitsh::LineEditor.special_prefixes -> string
 *
 * Gets the list of characters that are word break characters, but
 * should be left in text when it is passed to the completion
 * function.
 *
 * See GNU Readline's rl_special_prefixes variable.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_get_special_prefixes(VALUE self)
{
    VALUE str;
    if (rl_special_prefixes == NULL) return Qnil;
    str = rb_ivar_get(mLineEditor, id_special_prefixes);
    if (!NIL_P(str)) {
        str = rb_str_dup_frozen(str);
        rb_obj_reveal(str, rb_cString);
    }
    return str;
}
#else
#define readline_s_set_special_prefixes rb_f_notimplement
#define readline_s_get_special_prefixes rb_f_notimplement
#endif

#ifdef HAVE_RL_COMPLETER_QUOTE_CHARACTERS
/*
 * call-seq:
 *   Gitsh::LineEditor.completer_quote_characters = string
 *
 * Sets a list of characters which can be used to quote a substring of
 * the line. Completion occurs on the entire substring, and within
 * the substring Gitsh::LineEditor.completer_word_break_characters are treated
 * as any other character, unless they also appear within this list.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_set_completer_quote_characters(VALUE self, VALUE str)
{
    static char *completer_quote_characters = NULL;

    OutputStringValue(str);
    if (completer_quote_characters == NULL) {
        completer_quote_characters =
            ALLOC_N(char, RSTRING_LEN(str) + 1);
    }
    else {
        REALLOC_N(completer_quote_characters, char, RSTRING_LEN(str) + 1);
    }
    strncpy(completer_quote_characters, RSTRING_PTR(str), RSTRING_LEN(str));
    completer_quote_characters[RSTRING_LEN(str)] = '\0';
    rl_completer_quote_characters = completer_quote_characters;

    return self;
}
#else
#define readline_s_set_completer_quote_characters rb_f_notimplement
#endif

#ifdef HAVE_RL_COMPLETER_QUOTE_CHARACTERS
/*
 * call-seq:
 *   Gitsh::LineEditor.completer_quote_characters -> string
 *
 * Gets a list of characters which can be used to quote a substring of
 * the line.
 *
 * Raises NotImplementedError if the using readline library does not support.
 */
static VALUE
readline_s_get_completer_quote_characters(VALUE str)
{
    if (rl_completer_quote_characters == NULL)
        return Qnil;
    return rb_locale_str_new_cstr(rl_completer_quote_characters);
}
#else
#define readline_s_get_completer_quote_characters rb_f_notimplement
#endif

#ifdef HAVE_RL_REFRESH_LINE
/*
 * call-seq:
 *   Gitsh::LineEditor.refresh_line -> nil
 *
 * Clear the current input line.
 */
static VALUE
readline_s_refresh_line(VALUE self)
{
    prepare_readline();
    rl_refresh_line(0, 0);
    return Qnil;
}
#else
#define readline_s_refresh_line rb_f_notimplement
#endif

static VALUE
hist_to_s(VALUE self)
{
    return rb_str_new_cstr("HISTORY");
}

static int
history_get_offset_history_base(int offset)
{
    return history_base + offset;
}

static int
history_get_offset_0(int offset)
{
    return offset;
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
        entry = history_get(history_get_offset_func(i));
    }
    if (entry == NULL) {
        rb_raise(rb_eIndexError, "invalid index");
    }
    return rb_locale_str_new_cstr(entry->line);
}

#ifdef HAVE_REPLACE_HISTORY_ENTRY
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
        entry = replace_history_entry(history_replace_offset_func(i), RSTRING_PTR(str), NULL);
    }
    if (entry == NULL) {
        rb_raise(rb_eIndexError, "invalid index");
    }
    return str;
}
#else
#define hist_set rb_f_notimplement
#endif

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
#ifdef HAVE_REMOVE_HISTORY
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
#else
    rb_notimplement();

    UNREACHABLE;
#endif
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
        entry = history_get(history_get_offset_func(i));
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

#ifdef HAVE_CLEAR_HISTORY
static VALUE
hist_clear(VALUE self)
{
    clear_history();
    return self;
}
#else
#define hist_clear rb_f_notimplement
#endif

static VALUE
filename_completion_proc_call(VALUE self, VALUE str)
{
    VALUE result;
    char **matches;
    int i;

    matches = rl_completion_matches(StringValuePtr(str),
                                    rl_filename_completion_function);
    if (matches) {
        result = rb_ary_new();
        for (i = 0; matches[i]; i++) {
            rb_ary_push(result, rb_locale_str_new_cstr(matches[i]));
            free(matches[i]);
        }
        free(matches);
        if (RARRAY_LEN(result) >= 2)
            rb_ary_shift(result);
    }
    else {
        result = Qnil;
    }
    return result;
}

static VALUE
username_completion_proc_call(VALUE self, VALUE str)
{
    VALUE result;
    char **matches;
    int i;

    matches = rl_completion_matches(StringValuePtr(str),
                                    rl_username_completion_function);
    if (matches) {
        result = rb_ary_new();
        for (i = 0; matches[i]; i++) {
            rb_ary_push(result, rb_locale_str_new_cstr(matches[i]));
            free(matches[i]);
        }
        free(matches);
        if (RARRAY_LEN(result) >= 2)
            rb_ary_shift(result);
    }
    else {
        result = Qnil;
    }
    return result;
}

void
Init_line_editor_native(void)
{
    VALUE history, fcomp, ucomp, version;

    /* Allow conditional parsing of the ~/.inputrc file. */
    rl_readline_name = (char *)"gitsh";

#if defined HAVE_RL_GETC_FUNCTION
    /* libedit check rl_getc_function only when rl_initialize() is called, */
    /* and using_history() call rl_initialize(). */
    /* This assignment should be placed before using_history() */
    rl_getc_function = readline_getc;
#elif defined HAVE_RL_EVENT_HOOK
    rl_event_hook = readline_event;
#endif

    rl_char_is_quoted_p = &readline_char_is_quoted;

    using_history();

    completion_proc = rb_intern(COMPLETION_PROC);
    completion_case_fold = rb_intern(COMPLETION_CASE_FOLD);
    quoting_detection_proc = rb_intern(QUOTING_DETECTION_PROC);
#if defined(HAVE_RL_PRE_INPUT_HOOK)
    id_pre_input_hook = rb_intern("pre_input_hook");
#endif
#if defined(HAVE_RL_SPECIAL_PREFIXES)
    id_special_prefixes = rb_intern("special_prefixes");
#endif

    mGitsh = rb_define_module("Gitsh");
    mLineEditor = rb_define_module_under(mGitsh, "LineEditor");
    rb_define_module_function(mLineEditor, "readline",
                              readline_readline, -1);
    rb_define_singleton_method(mLineEditor, "input=",
                               readline_s_set_input, 1);
    rb_define_singleton_method(mLineEditor, "output=",
                               readline_s_set_output, 1);
    rb_define_singleton_method(mLineEditor, "completion_proc=",
                               readline_s_set_completion_proc, 1);
    rb_define_singleton_method(mLineEditor, "completion_proc",
                               readline_s_get_completion_proc, 0);
    rb_define_singleton_method(mLineEditor, "quoting_detection_proc=",
                               readline_s_set_quoting_detection_proc, 1);
    rb_define_singleton_method(mLineEditor, "quoting_detection_proc",
                               readline_s_get_quoting_detection_proc, 0);
    rb_define_singleton_method(mLineEditor, "completion_case_fold=",
                               readline_s_set_completion_case_fold, 1);
    rb_define_singleton_method(mLineEditor, "completion_case_fold",
                               readline_s_get_completion_case_fold, 0);
    rb_define_singleton_method(mLineEditor, "line_buffer",
                               readline_s_get_line_buffer, 0);
    rb_define_singleton_method(mLineEditor, "point",
                               readline_s_get_point, 0);
    rb_define_singleton_method(mLineEditor, "point=",
                               readline_s_set_point, 1);
    rb_define_singleton_method(mLineEditor, "set_screen_size",
                               readline_s_set_screen_size, 2);
    rb_define_singleton_method(mLineEditor, "get_screen_size",
                               readline_s_get_screen_size, 0);
    rb_define_singleton_method(mLineEditor, "vi_editing_mode",
                               readline_s_vi_editing_mode, 0);
    rb_define_singleton_method(mLineEditor, "vi_editing_mode?",
                               readline_s_vi_editing_mode_p, 0);
    rb_define_singleton_method(mLineEditor, "emacs_editing_mode",
                               readline_s_emacs_editing_mode, 0);
    rb_define_singleton_method(mLineEditor, "emacs_editing_mode?",
                               readline_s_emacs_editing_mode_p, 0);
    rb_define_singleton_method(mLineEditor, "completion_append_character=",
                               readline_s_set_completion_append_character, 1);
    rb_define_singleton_method(mLineEditor, "completion_append_character",
                               readline_s_get_completion_append_character, 0);
    rb_define_singleton_method(mLineEditor, "completion_suppress_quote=",
                               readline_s_set_completion_supress_quote, 1);
    rb_define_singleton_method(mLineEditor, "completion_suppress_quote",
                               readline_s_get_completion_supress_quote, 0);
    rb_define_singleton_method(mLineEditor, "completion_quote_character",
                               readline_s_get_completion_quote_character, 0);
    rb_define_singleton_method(mLineEditor, "completer_word_break_characters=",
                               readline_s_set_completer_word_break_characters, 1);
    rb_define_singleton_method(mLineEditor, "completer_word_break_characters",
                               readline_s_get_completer_word_break_characters, 0);
    rb_define_singleton_method(mLineEditor, "completer_quote_characters=",
                               readline_s_set_completer_quote_characters, 1);
    rb_define_singleton_method(mLineEditor, "completer_quote_characters",
                               readline_s_get_completer_quote_characters, 0);
    rb_define_singleton_method(mLineEditor, "refresh_line",
                               readline_s_refresh_line, 0);
    rb_define_singleton_method(mLineEditor, "pre_input_hook=",
                               readline_s_set_pre_input_hook, 1);
    rb_define_singleton_method(mLineEditor, "pre_input_hook",
                               readline_s_get_pre_input_hook, 0);
    rb_define_singleton_method(mLineEditor, "insert_text",
                               readline_s_insert_text, 1);
    rb_define_singleton_method(mLineEditor, "delete_bytes",
                               readline_s_delete_bytes, 2);
    rb_define_singleton_method(mLineEditor, "redisplay",
                               readline_s_redisplay, 0);
    rb_define_singleton_method(mLineEditor, "special_prefixes=",
                               readline_s_set_special_prefixes, 1);
    rb_define_singleton_method(mLineEditor, "special_prefixes",
                               readline_s_get_special_prefixes, 0);

#if USE_INSERT_IGNORE_ESCAPE
    CONST_ID(id_orig_prompt, "orig_prompt");
    CONST_ID(id_last_prompt, "last_prompt");
#endif

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

    /*
     * The history buffer. It extends Enumerable module, so it behaves
     * just like an array.
     * For example, gets the fifth content that the user input by
     * HISTORY[4].
     */
    rb_define_const(mLineEditor, "HISTORY", history);

    fcomp = rb_obj_alloc(rb_cObject);
    rb_define_singleton_method(fcomp, "call",
                               filename_completion_proc_call, 1);
    /*
     * The Object with the call method that is a completion for filename.
     * This is sets by Gitsh::LineEditor.completion_proc= method.
     */
    rb_define_const(mLineEditor, "FILENAME_COMPLETION_PROC", fcomp);

    ucomp = rb_obj_alloc(rb_cObject);
    rb_define_singleton_method(ucomp, "call",
                               username_completion_proc_call, 1);
    /*
     * The Object with the call method that is a completion for usernames.
     * This is sets by Gitsh::LineEditor.completion_proc= method.
     */
    rb_define_const(mLineEditor, "USERNAME_COMPLETION_PROC", ucomp);
    history_get_offset_func = history_get_offset_history_base;
    history_replace_offset_func = history_get_offset_0;
#if defined HAVE_RL_LIBRARY_VERSION
    version = rb_str_new_cstr(rl_library_version);
#if defined HAVE_CLEAR_HISTORY || defined HAVE_REMOVE_HISTORY
    if (strncmp(rl_library_version, EDIT_LINE_LIBRARY_VERSION,
                strlen(EDIT_LINE_LIBRARY_VERSION)) == 0) {
        add_history("1");
        if (history_get(history_get_offset_func(0)) == NULL) {
            history_get_offset_func = history_get_offset_0;
        }
#ifdef HAVE_REPLACE_HISTORY_ENTRY
        if (replace_history_entry(0, "a", NULL) == NULL) {
            history_replace_offset_func = history_get_offset_history_base;
        }
#endif
#ifdef HAVE_CLEAR_HISTORY
        clear_history();
#else
        {
            HIST_ENTRY *entry = remove_history(0);
            if (entry) {
                free((char *)entry->line);
                free(entry);
            }
        }
#endif
    }
#endif
#else
    version = rb_str_new_cstr("2.0 or prior version");
#endif
    /* Version string of GNU Readline or libedit. */
    rb_define_const(mLineEditor, "VERSION", version);

    rl_attempted_completion_function = readline_attempted_completion_function;
#if defined(HAVE_RL_PRE_INPUT_HOOK)
    rl_pre_input_hook = (rl_hook_func_t *)readline_pre_input_hook;
#endif
#ifdef HAVE_RL_CATCH_SIGNALS
    rl_catch_signals = 0;
#endif
#ifdef HAVE_RL_CLEAR_SIGNALS
    rl_clear_signals();
#endif

    rb_gc_register_address(&readline_instream);
    rb_gc_register_address(&readline_outstream);
}
