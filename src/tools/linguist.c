/*
 * linguist.c
 *
 *
 * Copyright 2014, 2015, 2016 John C. Bollinger
 *
 *
 * This file is part of the CIF API.
 *
 * The CIF API is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The CIF API is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the CIF API.  If not, see <http://www.gnu.org/licenses/>.
 */

/* the package's config.h is not needed or used */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <unicode/ustdio.h>
#include <unicode/ustring.h>
#include "cif.h"
#include "cif_error.h"

/* data types */

typedef enum { CIF11 = 0, CIF20 = 1, STAR20 = 2, NONE } format;

struct ws_node {
    UChar *ws;
    struct ws_node *next_piece;
    struct ws_node *next_run;
};

struct context {
    const char *progname;
    UFILE *out;
    UFILE *ustderr;
    const char *out_encoding;
    const char *element_separator;
    UChar *extra_ws;
    UChar *extra_eol;
    int no_fold11_output;
    int prefix11_output;
    int quiet;
    int halt_on_error;
    format input_format;
    format output_format;
    unsigned error_count;
    int at_start;
    int in_container;
    int in_loop;
    int in_ws_run;
    int column;
    int synthesize_packet;
    struct ws_node *ws_queue;
};

/* macros */

#ifdef __GNUC__
#define UNUSED   __attribute__ ((__unused__))
#else
#define UNUSED
#endif

#ifdef _WIN32
#define FILE_SEP '\\'
#else
#define FILE_SEP '/'
#endif

#define DEFAULT_OUTPUT_FORMAT "cif20"
#define MAX_LINE_LENGTH 2048

#define UCHAR_TAB   ((UChar) 0x09)
#define UCHAR_LF    ((UChar) 0x0a)
#define UCHAR_CR    ((UChar) 0x0d)
#define UCHAR_SP    ((UChar) 0x20)
#define UCHAR_HASH  ((UChar) 0x23)
#define UCHAR_DOT   ((UChar) 0x2e)
#define UCHAR_COLON ((UChar) 0x3a)
#define UCHAR_SEMI  ((UChar) 0x3b)
#define UCHAR_QUERY ((UChar) 0x3f)
#define UCHAR_C     ((UChar) 0x43)
#define UCHAR_F     ((UChar) 0x46)
#define UCHAR_I     ((UChar) 0x49)
#define UCHAR_a     ((UChar) 0x61)
#define UCHAR_e     ((UChar) 0x65)
#define UCHAR_l     ((UChar) 0x6c)
#define UCHAR_o     ((UChar) 0x6f)
#define UCHAR_p     ((UChar) 0x70)
#define UCHAR_s     ((UChar) 0x73)
#define UCHAR_v     ((UChar) 0x76)
#define UCHAR_OBRK  ((UChar) 0x5b)
#define UCHAR_BSL   ((UChar) 0x5c)
#define UCHAR_CBRK  ((UChar) 0x5d)
#define UCHAR_UNDER ((UChar) 0x5f)
#define UCHAR_OBRC  ((UChar) 0x7b)
#define UCHAR_CBRC  ((UChar) 0x7d)

/* The text prefixed used by this program when one is required */
#define PREFIX "> "

/*
 * The halfwidth of the window within which the line-folding algorithm will look for a suitable location to fold long
 * lines.
 */
#define FOLD_WINDOW 8

/*
 * The maximum length of the data content of any physical line in a line-folded text field.  Such lines must also
 * contain a fold separator at the end (minimum one character, not including a line terminator.
 */
#define MAX_FOLD_LENGTH (MAX_LINE_LENGTH - 1)

#define SPACE_FORBIDDEN -1
#define SPACE_ALLOWED    0
#define SPACE_REQUIRED   1

#define CONTEXT_PROGNAME(c) (((struct context *)(c))->progname)
#define CONTEXT_SET_OUTFORMAT(c, f) do { ((struct context *)(c))->output_format = (f); } while (0)
#define CONTEXT_SET_SEPARATOR(c, s) do { ((struct context *)(c))->element_separator = (s); } while (0)
#define CONTEXT_IN_CONTAINER(c) ((c)->in_container)
#define CONTEXT_PUSH_CONTAINER(c) do { (c)->in_container += 1; } while (0)
#define CONTEXT_POP_CONTAINER(c) do {  (c)->in_container -= 1; } while (0)

/*
 * A macro for handling short options that accept arguments
 */
#define PROCESS_ARGS(arg, opts, i, j) do {                  \
    if (argv[i][++j]) {                                     \
        process_args_ ## arg ((opts), argv[i] + j);         \
    } else if ((i + 1 < argc) && (argv[i + 1][0] != '-')) { \
        process_args_ ## arg ((opts), argv[++i]);           \
    } else {                                                \
        process_args_ ## arg ((opts), NULL);                \
    }                                                       \
} while (0)

#define DIE_UNLESS(p, n) do { \
    if (!(p)) {               \
        perror(n);            \
        exit(2);              \
    }                         \
} while(0)

/* global variables */

const char headers[][16] = {
        "#\\#CIF_1.1\n",  /* CIF11  */
        "#\\#CIF_2.0\n",  /* CIF20  */
        ""                /* STAR20 */
};

/* Do not reorder the elements of these arrays, and if new elements are ever added then put them at the end */
const UChar standard_ws_chars[] = { UCHAR_SP, UCHAR_TAB, 0 };
const UChar standard_eol_chars[] = { UCHAR_LF, UCHAR_CR, 0 };

/* function prototypes */

/* option handling */
void usage(const char *progname);
void process_args(int argc, char *argv[], struct cif_parse_opts_s *parse_opts);
void process_args_input_encoding(struct cif_parse_opts_s *parse_opts, const char *encoding);
void process_args_input_format(struct cif_parse_opts_s *parse_opts, const char *format);
void process_args_input_folding(struct cif_parse_opts_s *parse_opts, const char *folding);
void process_args_input_prefixing(struct cif_parse_opts_s *parse_opts, const char *prefixing);
void process_args_output_encoding(struct cif_parse_opts_s *parse_opts, const char *encoding);
void process_args_output_format(struct cif_parse_opts_s *parse_opts, const char *format);
void process_args_output_folding(struct cif_parse_opts_s *parse_opts, const char *folding);
void process_args_output_prefixing(struct cif_parse_opts_s *parse_opts, const char *prefixing);
void process_args_quiet(struct cif_parse_opts_s *parse_opts);
void process_args_strict(struct cif_parse_opts_s *parse_opts);
int to_boolean(const char *val);

/* parser callbacks */
int print_header(cif_tp *cif, void *context);
int handle_cif_end(cif_tp *cif, void *context);
int open_block(cif_container_tp *block, void *context);
int open_frame(cif_container_tp *frame, void *context);
int finish_frame(cif_container_tp *container, void *context);
int flush_container(cif_container_tp *container, void *context);
int handle_loop_start(cif_loop_tp *loop, void *context);
int handle_loop_end(cif_loop_tp *loop, void *context);
int handle_packet_start(cif_packet_tp *packet, void *context);
int discard_packet(cif_packet_tp *packet, void *context);
int print_item(UChar *name, cif_value_tp *value, void *context);
void preserve_whitespace(size_t line, size_t column, const UChar *ws, size_t length, void *data);
int error_callback(int code, size_t line, size_t column, const UChar *text, size_t length, void *data);

/* output, utility, and helper functions */
void consume_version_comment(struct context *context);
int print_code(cif_container_tp *container, struct context *context, const char *type);
int flush_loops(cif_container_tp *container);
int print_value(cif_value_tp *value, struct context *context, unsigned int ws_needed_max);
int print_list(cif_value_tp *value, struct context *context);
int print_table(cif_value_tp *value, struct context *context);
int print_value_text(cif_value_tp *value, struct context *context, unsigned int ws_needed);
int print_text_field(UChar *str, int do_fold, int do_prefix, struct context *context);
ptrdiff_t compute_fold_length(UChar *fold_start, ptrdiff_t line_length, ptrdiff_t target_length, int window,
        int allow_folding_before_semi);
int32_t print_ws_run(struct context *context);
int32_t print_all_ws_runs(struct context *context);
int ensure_space(int minimum_space, int data_length, struct context *context);
int print_u_literal(int preceding_space, const UChar *str, int line1_length, struct context *context);
int print_delimited(UChar *str, UChar *delim, int delim_length, UFILE *out);
void translate_whitespace(UChar *text, size_t length, const UChar *extra_eol, const UChar *extra_ws);
void flush_ws(struct ws_node *start);
struct ws_node *free_ws_node(struct ws_node *node);

/* function implementations */

void usage(const char *progname) {
    fprintf(stderr, "\nusage: %s [-f <input-format>] [-e <input-encoding>] [-l [1|0]] [-p [1|0]]\n"
                    "          [-F <output-format>] [-E <output-encoding>] [-L [1|0]] [-P [1|0]]\n"
                    "          [-q] [-s] [--] [<input-file> [<output-file>]]\n\n", progname);
    fputs(          "Description:\n", stderr);
    fputs(       /* "Transforms CIF data among CIF formats and dialects, or to STAR 2.0 format.\n" */
                    "Transforms CIF data among CIF formats and dialects.\n"
                    "If no input file is specified, or if input is specified as \"-\", then input\n"
                    "is read from the standard input, else it is from the specified file.  If no\n"
                    "output file is specified, or if output is specified as \"-\", then output is\n"
                    "directed to the standard output, else it goes to the specified file.\n\n",
                    stderr);
    fputs(          "Options that take boolean arguments (described as 1|0 in the synopsis and option\n"
                    "descriptions) will also accept arguments 'yes', 'true', 'no', and 'false'.\n\n",
                    stderr);
    fputs(          "Options:\n",
                    stderr);
    fputs(          "  -e <encoding>, --input-encoding=<encoding>\n"
                    "          Specifies the input character encoding.  If given as \"auto\" (the\n"
                    "          default) then the program attempts to determine the encoding from the\n"
                    "          input and falls back to a format- and system-specific default if it is\n",
                    stderr);
    fputs(          "          unable to do so.  Otherwise, the encoding names recognized are system-\n"
                    "          dependent, but they take the form of IANA names and aliases.  The specified\n"
                    "          encoding will be used, even for CIF 2.0 format input (even though the CIF 2.0\n"
                    "          specifications permit only UTF-8).\n\n",
                    stderr);
    fputs(          "  -E <encoding>, --output-encoding=<encoding>\n"
                    "          Specifies the output character encoding.  If given as \"auto\" (the\n"
                    "          default) then the program chooses an encoding in a format- and system-specific\n"
                    "          way.  Otherwise, the encoding names recognized are system-\n"
                    "          dependent, but they take the form of IANA names and aliases.  The specified\n"
                    "          encoding will be used, even for CIF 2.0 format output (even though the CIF 2.0\n"
                    "          specifications permit only UTF-8).\n\n",
                    stderr);
    fputs(          "  -f <format>, --input-format=<format>\n"
                    "          Specifies the input format.  The formats supported are \"auto\" (the\n"
                    "          program guesses; this is the default), \"cif10\" (the program assumes\n"
                    "          CIF 1.0), \"cif11\" (the program assumes CIF 1.1), and \"cif20\" (the\n"
                    "          program assumes CIF 2.0).  A format (other than auto) specified via this\n"
                    "          option overrides any contradictory indications in the file itself.\n\n",
                    stderr);
    fputs(          "  -F <format>, --output-format=<format>\n"
                    "          Specifies the output format.  The formats supported are \"cif11\" (the\n"
                    "          program emits CIF 1.1 format) and \"cif20\" (the program emits CIF 2.0\n"
                    "          format; this is the default).\n\n",
                    stderr);
    fputs(          "  -l 1|0, --input-line-folding=1|0\n"
                    "          Specifies whether to recognize and decode the CIF line-folding protocol\n"
                    "          in text fields in the the input.  Defaults to 1 (yes).\n\n",
                    stderr);
    fputs(          "  -L 1|0, --output-line-folding=1|0\n"
                    "          Specifies whether to allow line folding of text fields in the output.\n"
                    "          The program chooses automatically, on a field-by-field basis, whether\n"
                    "          to perform folding.  Defaults to 1 (yes).\n\n",
                    stderr);

    fputs(          "  -p 0|1, --input-text-prefixing=0|1\n"
                    "          Specifies whether to recognize and decode the CIF text-prefixing protocol\n"
                    "          in text fields in the the input.  Defaults to 1 (yes).\n\n",
                    stderr);
    fputs(          "  -P 0|1, --output-text-prefixing=0|1\n"
                    "          Specifies whether to allow line prefixing of text fields in the output.\n"
                    "          The program chooses automatically, on a field-by-field basis, whether\n"
                    "          to perform prefixing.  Defaults to 1 (yes).\n\n",
                    stderr);
    fputs(          "  -q      This option suppresses diagnostic output.  The exit status will still\n"
                    "          provide a general idea of the program's success.\n\n",
                    stderr);
    fputs(          "  -s      This option instructs the program to insist that the input data strictly\n"
                    "          conform to the chosen CIF format.  Any error will cause the program to\n"
                    "          terminate prematurely.  If this option is not given then the program will\n"
                    "          instead make a best effort at reading and processing the input despite\n"
                    "          any errors it may encounter.  Such error recovery efforts are inherently\n"
                    "          uncertain, however, and sometimes lossy.\n\n",
                    stderr);
    fputs(          "  --      Indicates the end of the option arguments.  Any subsequent arguments are\n"
                    "          interpreted as file names.\n\n",
                    stderr);

    fputs(          "Exit Status:\n"
                    "The program exits with status 0 if the input was parsed without any error and\n"
                    "successfully transformed.  It exits with status 1 if parse errors were detected,\n"
                    "but the program nevertheless consumed the entire input and produced a\n"
                    "transformation.  It exits with status 2 if no parse was attempted.  It exits with\n"
                    "status 3 if parse or transformation is interrupted prior to the full input being\n"
                    "consumed.\n\n"
                    , stderr);
    exit(2);
}

/*
 * Returns 1 if the provided string represents truth, 0 if it represents falsehood, or -1 if it is unrecognized
 */
int to_boolean(const char *val) {
    size_t len = strlen(val);
    char *dup = (char *) malloc((len + 1) * sizeof(char));
    int result;

    if (dup) {
        char *c;

        strcpy(dup, val);
        for (c = dup; *c; c += 1) {
            *c = tolower(*c);
        }
        val = dup;
    }

    if (len > 5) {
        /* the maximum-length recognized value is "false", with length 5.  Fail now if the specified value is longer */
        result = -1;
    } else if (!(strcmp(val, "1") && strcmp(val, "yes") && strcmp(val, "true"))) {
        result = 1;
    } else if (!(strcmp(val, "0") && strcmp(val, "no") && strcmp(val, "false"))) {
        result = 0;
    } else {
        result = -1;
    }

    free(dup);
    return result;
}

void process_args_input_encoding(struct cif_parse_opts_s *parse_opts, const char *encoding) {
    if (!encoding) {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    }
    if (strcmp(encoding, "auto")) {
        parse_opts->default_encoding_name = NULL;
        parse_opts->force_default_encoding = 0;
    } else {
        parse_opts->default_encoding_name = encoding;
        parse_opts->force_default_encoding = 1;
    }
}

void process_args_input_format(struct cif_parse_opts_s *parse_opts, const char *fmt) {
    if (!fmt) {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    }
    if (!strcmp(fmt, "auto")) {
        parse_opts->prefer_cif2 = 0;
    } else if (!strcmp(fmt, "cif20")) {
        parse_opts->prefer_cif2 = 20;
    } else if (!strcmp(fmt, "cif11")) {
        parse_opts->prefer_cif2 = -1;
    } else if (!strcmp(fmt, "cif10")) {
        parse_opts->prefer_cif2 = -1;
        parse_opts->extra_ws_chars = "\v";
        parse_opts->extra_eol_chars = "\f";
    } else {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    }
}

void process_args_input_folding(struct cif_parse_opts_s *parse_opts, const char *folding) {
    /* if the optional argument is not specified then it is taken as 1/true/yes */
    int argval = folding ? to_boolean(folding) : 1;

    if (argval < 0) {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    } else {
        /* +1 for true, -1 for false */
        parse_opts->line_folding_modifier = 2 * argval - 1;
    }
}

void process_args_input_prefixing(struct cif_parse_opts_s *parse_opts, const char *prefixing) {
    /* if the optional argument is not specified then it is taken as 1/true/yes */
    int argval = prefixing ? to_boolean(prefixing) : 1;

    if (argval < 0) {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    } else {
        /* +1 for true, -1 for false */
        parse_opts->text_prefixing_modifier = 2 * argval - 1;
    }
}

void process_args_output_encoding(struct cif_parse_opts_s *parse_opts, const char *encoding) {
    if (!encoding) {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    }
    ((struct context *) (parse_opts->user_data))->out_encoding = encoding;
}

void process_args_output_format(struct cif_parse_opts_s *parse_opts, const char *fmt) {
    if (!fmt) {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    } else if (!strcmp(fmt, "cif11")  || !strcmp(fmt, "cif1.1")) {
        CONTEXT_SET_OUTFORMAT(parse_opts->user_data, CIF11);
        CONTEXT_SET_SEPARATOR(parse_opts->user_data, NULL);
    } else if (!strcmp(fmt, "cif20")  || !strcmp(fmt, "cif2.0")) {
        CONTEXT_SET_OUTFORMAT(parse_opts->user_data, CIF20);
        CONTEXT_SET_SEPARATOR(parse_opts->user_data, "");
/*
    } else if (!strcmp(fmt, "star20") || !strcmp(fmt, "star2.0")) {
        CONTEXT_SET_OUTFORMAT(parse_opts->user_data, STAR20);
        CONTEXT_SET_SEPARATOR(parse_opts->user_data, ",");
 */
    } else {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    }
}

void process_args_output_folding(struct cif_parse_opts_s *parse_opts, const char *folding) {
    /* if the optional argument is not specified then it is taken as 1/true/yes */
    int argval = folding ? to_boolean(folding) : 1;

    if (argval < 0) {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    } else {
        assert(argval < 2);
        ((struct context *) parse_opts->user_data)->no_fold11_output = 1 - argval;
    }
}

void process_args_output_prefixing(struct cif_parse_opts_s *parse_opts, const char *prefixing) {
    /* if the optional argument is not specified then it is taken as 1/true/yes */
    int argval = prefixing ? to_boolean(prefixing) : 1;

    if (argval < 0) {
        usage(CONTEXT_PROGNAME(parse_opts->user_data));
    } else {
        ((struct context *) parse_opts->user_data)->no_fold11_output = argval;
    }
}

void process_args_quiet(struct cif_parse_opts_s *parse_opts) {
    ((struct context *) parse_opts->user_data)->quiet = 1;
}

void process_args_strict(struct cif_parse_opts_s *parse_opts) {
    ((struct context *) parse_opts->user_data)->halt_on_error = 1;
}

void process_args(int argc, char *argv[], struct cif_parse_opts_s *parse_opts) {
    struct context *context = (struct context *) parse_opts->user_data;
    int infile_index = 0;
    int outfile_index = 0;
    char *c;
    int i;

    assert(argc > 0);

    c = strrchr(argv[0], FILE_SEP);  /* distinguish the program's simple name from any path component */
    context->progname = (c ? (c + 1) : argv[0]);
    parse_opts->whitespace_callback = preserve_whitespace;
    parse_opts->error_callback = error_callback;
    process_args_output_format(parse_opts, DEFAULT_OUTPUT_FORMAT);

    for (i = 1; i < argc; i += 1) {
        if ((argv[i][0] != '-') || (!argv[i][1])) {
            /* a non-option argument */
            break;
        } else if (argv[i][1] == '-') {
            if (argv[i][2]) {
                /* a GNU-style long option */
                int j;

                for (j = 2; argv[i][j] && argv[i][j] != '='; ) {
                    j += 1;
                }
                if (!strncmp(&argv[i][2], "input-format", j - 2)) {
                    process_args_input_format(parse_opts, (argv[i][j] ? argv[i] + j + 1 : NULL));
                } else if (!strncmp(&argv[i][2], "input-encoding", j - 2)) {
                    process_args_input_encoding(parse_opts, (argv[i][j] ? argv[i] + j + 1 : NULL));
                } else if (!strncmp(&argv[i][2], "input-line-folding", j - 2)) {
                    process_args_input_folding(parse_opts, (argv[i][j] ? argv[i] + j + 1 : NULL));
                } else if (!strncmp(&argv[i][2], "input-text-prefixing", j - 2)) {
                    process_args_input_prefixing(parse_opts, (argv[i][j] ? argv[i] + j + 1 : NULL));
                } else if (!strncmp(&argv[i][2], "output-format", j - 2)) {
                    process_args_output_format(parse_opts, (argv[i][j] ? argv[i] + j + 1 : NULL));
                } else if (!strncmp(&argv[i][2], "output-encoding", j - 2)) {
                    process_args_output_encoding(parse_opts, (argv[i][j] ? argv[i] + j + 1 : NULL));
                } else if (!strncmp(&argv[i][2], "output-line-folding", j - 2)) {
                    process_args_output_folding(parse_opts, (argv[i][j] ? argv[i] + j + 1 : NULL));
                } else if (!strncmp(&argv[i][2], "output-text-prefixing", j - 2)) {
                    process_args_output_prefixing(parse_opts, (argv[i][j] ? argv[i] + j + 1 : NULL));
                } else if (!strcmp(&argv[i][2], "quiet")) {
                    process_args_quiet(parse_opts);
                } else if (!strcmp(&argv[i][2], "strict")) {
                    process_args_strict(parse_opts);
                } else {
                    usage(context->progname);
                }
            } else {
                /* explicit end of options */
                i += 1;
                break;
            }
        } else {
            int j;

            /* process short options */
            for (j = 1; ; j += 1) {
                switch (argv[i][j]) {
                    case '\0':
                        if (j == 1) {
                            /* '-' by itself is a file designator, not an option */
                            goto end_options;
                        }
                        goto end_arg;
                    case 'f':
                        PROCESS_ARGS(input_format, parse_opts, i, j);
                        goto end_arg;
                    case 'e':
                        PROCESS_ARGS(input_encoding, parse_opts, i, j);
                        goto end_arg;
                    case 'l':
                        PROCESS_ARGS(input_folding, parse_opts, i, j);
                        goto end_arg;
                    case 'p':
                        PROCESS_ARGS(input_prefixing, parse_opts, i, j);
                        goto end_arg;
                    case 'F':
                        PROCESS_ARGS(output_format, parse_opts, i, j);
                        goto end_arg;
                    case 'E':
                        PROCESS_ARGS(output_encoding, parse_opts, i, j);
                        goto end_arg;
                    case 'L':
                        PROCESS_ARGS(output_folding, parse_opts, i, j);
                        goto end_arg;
                    case 'P':
                        PROCESS_ARGS(output_prefixing, parse_opts, i, j);
                        goto end_arg;
                    case 'q': /* quiet mode */
                        process_args_quiet(parse_opts);
                        break;
                    case 's': /* strict parsing mode */
                        process_args_strict(parse_opts);
                        break;
                    default:
                        usage(context->progname);
                        break;
                } /* switch */
            } /* for j */
            end_arg:
            ;
        } /* else */
    } /* for i */

    end_options:

    if (i < argc) infile_index = i++;
    if (i < argc) outfile_index = i++;
    if (i < argc) usage(context->progname);

    if (infile_index && strcmp(argv[infile_index], "-")) {
        /* Connect the specified input file to stdin */
        /* For consistency, use text mode even for CIF 2 / STAR 2, because the standard input comes that way */
        FILE *in = freopen(argv[infile_index], "r", stdin);
        DIE_UNLESS(in, context->progname);
    } /* else use the standard input provided by the environment */

    if (outfile_index && strcmp(argv[outfile_index], "-")) {
        /* Connect the specified output file to stdout */
        /* For consistency, use text mode even for CIF 2 / STAR 2, because the standard output comes that way */
        FILE *out = freopen(argv[outfile_index], "w", stdout);
        DIE_UNLESS(out, context->progname);
    } /* else use the standard output provided by the environment */

    context->out = u_finit(stdout, "C", context->out_encoding);
    context->ustderr = u_finit(stderr, NULL, NULL);

    if (!(context->out && context->ustderr)) {
        perror(context->progname);
        fprintf(stderr, "%s: could not initialize Unicode output and/or error stream\n", argv[0]);
        exit(2);
    }

    /* final adjustments */

    context->at_start = 1;

    if (parse_opts->extra_ws_chars && parse_opts->extra_ws_chars[0]) {
        context->extra_ws = (UChar *) malloc((strlen(parse_opts->extra_ws_chars) + 1) * sizeof(UChar));
        if (!context->extra_ws) {
            perror(context->progname);
            exit(2);
        }
        u_sprintf(context->extra_ws, "%s", parse_opts->extra_ws_chars);
    }

    if (parse_opts->extra_eol_chars && parse_opts->extra_eol_chars[0]) {
        context->extra_eol = (UChar *) malloc((strlen(parse_opts->extra_eol_chars) + 1) * sizeof(UChar));
        if (!context->extra_eol) {
            perror(context->progname);
            exit(2);
        }
        u_sprintf(context->extra_eol, "%s", parse_opts->extra_eol_chars);
    }
}

struct ws_node *free_ws_node(struct ws_node *node) {
    struct ws_node *next = node->next_piece;

    free(node->ws);
    free(node);

    return next;
}

/* Discards cached whitespace, starting at the specified node.  Does nothing if the argument is null. */
void flush_ws(struct ws_node *start) {
    while (start) {
        struct ws_node *next_run = start->next_run;
        struct ws_node *one_piece = start;

        while (one_piece) {
            one_piece = free_ws_node(one_piece);
        }

        start = next_run;
    }
}

/*
 * Prints and releases the next whitespace run stored in the specified context
 *
 * Returns the number of characters printed (which may be zero), or -1 if an I/O error occurs
 */
int32_t print_ws_run(struct context *context) {
    if (!context->ws_queue) {
        return 0;
    } else {
        struct ws_node *current = context->ws_queue;
        int32_t rval = 0;

        /* XXX: consider suppressing trailing non-comment whitespace */

        /* we need to do this now, before we free the first cache element: */
        context->ws_queue = context->ws_queue->next_run;

        while (current) {
            UChar *brk;
            int32_t uncounted = 0;
            int32_t nlines = 0;
            int32_t nprinted;

            assert(current->ws);

            /* determine how many characters occur on lines preceding the last one of the run */
            /* depends on any non-standard EOL chars to have been translated to standard ones */
            for (brk = current->ws; (brk = u_strpbrk(brk, standard_eol_chars)); ) {
                if ((*brk == UCHAR_CR) && (*(brk + 1) == UCHAR_LF)) {
                    brk += 2;
                } else {
                    brk += 1;
                }
                /* note: we assume here that the whitespace run is not longer than an int32_t can count */
                nlines += 1;
                uncounted = brk - current->ws;
            }

            /* FIXME: this may print whitespace / comments beyond the line length limit */
            if ((nprinted = u_fprintf(context->out, "%S", current->ws)) < uncounted) {
                return -1;
            } else {
                rval += nprinted;
                if (nlines) {
                    context->column = nprinted - uncounted;
                } else {
                    context->column += nprinted;
                }
            }

            current = free_ws_node(current);
        }

        return rval;
    }
}

/*
 * Prints and releases all whitespace runs stored in the specified context
 *
 * Returns the number of characters printed, which may be zero, or -1 if an error occurs
 */
int32_t print_all_ws_runs(struct context *context) {
    int32_t rval = 0;

    while (context->ws_queue) {
        int32_t run_length;

        if ((run_length = print_ws_run(context)) >= 0) {
            rval += run_length;
        } else {
            return -1;
        }
    }

    return rval;
}

/*
 * Consumes a leading CIF version comment if one has been cached in the specified context
 */
void consume_version_comment(struct context *context) {
    /*
     * This function is perhaps over-engineered, as it accounts for the possibility that an initial CIF version comment
     * was split over two or more calls to the whitespace callback.  The current parser will never do that, and it's
     * difficult to imagine a future version that might do.  Nevertheless, this code is written to the CIF API
     * _specification_, not to any particular implementation.
     */

    static const UChar cif_header_start[] = {
            UCHAR_HASH, UCHAR_BSL, UCHAR_HASH, UCHAR_C, UCHAR_I, UCHAR_F, UCHAR_UNDER, 0 };
    static const int check_length = sizeof(cif_header_start) / sizeof(cif_header_start[0]) - 1;

    /*
     * compare the lead part of any stored whitespace to the expected start of a CIF version header
     *
     * We need to consider only the pieces of the first run.  Under no circumstances will the header be spread over
     * multiple runs (that's under this program's direct control).
     */

    struct ws_node *this_piece;
    int checked = 0;

    for (this_piece = context->ws_queue; this_piece; this_piece = this_piece->next_piece) {
        UChar *next_char;

        for (next_char = this_piece->ws; *next_char; ) {
            if (*(next_char++) != cif_header_start[checked++]) {
                /* no match */
                return;
            } else if (checked >= check_length) {

                /* matched */

                struct ws_node *next_run = context->ws_queue->next_run;

                /* scan forward to the next line terminator */
                for (next_char = u_strpbrk(next_char, standard_eol_chars);
                        !next_char && this_piece->next_piece;
                        next_char = u_strpbrk(this_piece->ws, standard_eol_chars)) {
                    this_piece = this_piece->next_piece;
                }

                if (!next_char) {
                    /* discard the whole run */
                    for (this_piece = context->ws_queue; this_piece; ) {
                        this_piece = free_ws_node(this_piece);
                    }

                    context->ws_queue = next_run;
                } else {
                    assert(this_piece);

                    /* move the next_char pointer past the line terminator */
                    next_char += (((*next_char == UCHAR_CR) && (*(next_char + 1) == UCHAR_LF)) ? 2 : 1);

                    /* discard any leading pieces of this whitespace run */
                    while (context->ws_queue != this_piece) {
                        context->ws_queue = free_ws_node(context->ws_queue);
                        context->ws_queue->next_run = next_run;
                    }

                    /* discard the matched portion of the current piece */
                    memmove(this_piece->ws, next_char, (u_strlen(next_char) + 1) * sizeof(UChar));
                }

                /* done */
                return;
            } /* else still a possible match */
        }
    }

    /* if control reaches here then there was no match */
}

int ensure_space(int minimum_space, int data_length, struct context *context) {
    int result = CIF_OK;

    if (context->column > 0) {
        if ((minimum_space + data_length + context->column) > MAX_LINE_LENGTH) {
            if (u_fprintf(context->out, "\n") == 1) {
                context->column = 0;
            } else {
                result = CIF_ERROR;
            }
        } else if (minimum_space) {
            if (u_fprintf(context->out, " ") == 1) {
                context->column += 1;
            } else {
                result = CIF_ERROR;
            }
        } /* else no action is required */
    } /* else the next output goes to column 1, which automatically follows whitespace */

    return result;
}

/*
 * Prints a literal string to the output, possibly preceded by a newline or space.  Updates the context's current
 * column according to the specified length of the first line; callers will need to correct the column after printing
 * a multi-line string.
 *
 * preceding_space: the number of space characters to output before the string (ignored if the function chooses to
 *     output a newline instead)
 * str: the Unicode string to print
 * line1_length: the number of characters in the longest initial substring of str that does not contain a newline
 * context: a pointer to the relevant context object
 *
 * Returns a CIF API status code
 */
int print_u_literal(int preceding_space, const UChar *str, int line1_length, struct context *context) {
    int32_t nprinted;

    if (context->column) {
        int32_t nspace = ((preceding_space > 0) ? preceding_space : 0);
        int need_newline = (line1_length + context->column + nspace) > MAX_LINE_LENGTH;

        if (need_newline) {
            if (preceding_space < 0) {
                return CIF_OVERLENGTH_LINE;
            } else {
                if ((nprinted = u_fprintf(context->out, "\n%S", str)) < 1) {
                    return CIF_ERROR;
                }
                context->column = nprinted - 1;
            }
        } else {
            if ((nprinted = u_fprintf(context->out, "%*s%S", nspace, "", str)) < nspace) {
                return CIF_ERROR;
            }
            context->column += nprinted;
        }
    } else {
        /* already at the beginning of a line */
        if ((nprinted = u_fprintf(context->out, "%S", str)) < 0) {
            return CIF_ERROR;
        }
        context->column = nprinted;
    }

    return CIF_OK;
}

/* function intended to handle cif_start events */
int print_header(cif_tp *cif UNUSED, void *data) {
    /* print a format header, if one is appropriate */
    struct context *context = (struct context *) data;
    format fmt = context->output_format;
    int32_t nchars;

    assert(fmt < NONE);

    /* non-empty headers are expected to be newline-terminated */
    nchars = u_fprintf(context->out, "%s", headers[fmt]);
    if (nchars < 0) {
        return CIF_ERROR;
    } else {
        context->column = 0;
        return CIF_OK;
    }
}

/* handler for cif_end events */
int handle_cif_end(cif_tp *cif UNUSED, void *data) {
    struct context *context = (struct context *) data;

    /* if this CIF was empty then consume any version comment */
    if (context->at_start) {
        consume_version_comment(context);
        context->at_start = 0;
    }

    /* dump any trailing comments or whitespace */
    if (print_all_ws_runs(context) <= 0) {
        /* no captured trailing whitespace */
        u_fputc(UCHAR_LF, context->out);  /* ignore the result */
    }

    /* close the output */
    u_fclose(context->out);
    u_fclose(context->ustderr);

    return 0;
}

/*
 * Outputs a data block or save frame header with the specified code
 */
int print_code(cif_container_tp *container, struct context *context, const char *type) {
    UChar *code;
    int32_t nchars;
    int result;

    if ((result = cif_container_get_code(container, &code)) == CIF_OK) {
        if (print_all_ws_runs(context)) {
            int32_t code_length = u_strlen(code);

            /*
             * Whitespace obtained from the context was printed.  No additional whitespace is needed or wanted, but if
             * for some reason the container header won't fit then ensure_space() will insert a newline.
             */
            if ((result = ensure_space(0, strlen(type) + code_length, context)) == CIF_OK) {
                if ((nchars = u_fprintf(context->out, "%s%S", type, code)) <= code_length) {
                    result = CIF_ERROR;
                } else {
                    context->column += nchars;
                    result = CIF_OK;
                }
            }
        } else {
            /* no whitespace was available from the context */
            if ((nchars = u_fprintf(context->out, "\n%s%S", type, code)) < 7) {
                result = CIF_ERROR;
            } else {
                context->column = nchars - 1;  /* don't count the leading newline */
                result = CIF_OK;
            }
        }

        free(code);
    }

    return result;
}

/*
 * Removes all data from the specified container
 */
int flush_loops(cif_container_tp *container) {
    int result;
    cif_loop_tp **loops;

    if ((result = cif_container_get_all_loops(container, &loops)) == CIF_OK) {
        cif_loop_tp **loop;

        for (loop = loops; *loop; loop += 1) {
            if ((result = cif_loop_destroy(*loop)) != CIF_OK) {
                break;
            }
        }

        /* clean up in case of error */
        for (; *loop; loop += 1) {
            cif_loop_free(*loop);
        }

        free(loops);
    }

    return result;
}

/* function intended to handle block_start events */
int open_block(cif_container_tp *block, void *data) {
    struct context *context = (struct context *) data;

    if (context->at_start) {
        consume_version_comment(context);
        context->at_start = 0;
    }
    context->in_ws_run = 0;
    CONTEXT_PUSH_CONTAINER(context);
    return print_code(block, context, "data_");
}

/*
 * Removes all save frames and data from the specified container.
 * Intended to handle block_end events, and to participate in handling frame_end events.
 */
int flush_container(cif_container_tp *container, void *data) {
    struct context *context = (struct context *) data;
    int result;
    cif_container_tp **frames;

    if ((result = cif_container_get_all_frames(container, &frames)) == CIF_OK) {
        cif_container_tp **frame;

        for (frame = frames; *frame; frame += 1) {
            if ((result = cif_container_destroy(*frame)) != CIF_OK) {

                /* clean up remaining frame handles */
                for (; *frame; frame += 1) {
                    cif_container_free(*frame);
                }

                goto done;
            }
        }

        /* all contained save frames were successfully destroyed; now destroy the loops */
        result = flush_loops(container);

        done:
        free(frames);
    }

    CONTEXT_POP_CONTAINER(context);

    return result;
}

/* function intended to handle frame_start events */
int open_frame(cif_container_tp *frame, void *data) {
    struct context *context = (struct context *) data;

    context->in_ws_run = 0;
    if (CONTEXT_IN_CONTAINER(context)) {
        CONTEXT_PUSH_CONTAINER(context);
        return print_code(frame, context, "save_");
    } else {
        return CIF_OK;
    }
}

/*
 * Outputs a save frame terminator and cleans out the frame contents
 * Intended to handle frame_end events.
 */
int finish_frame(cif_container_tp *container, void *data) {
    static const UChar term[] = { UCHAR_s, UCHAR_a, UCHAR_v, UCHAR_e, UCHAR_UNDER, 0 };
    struct context *context = (struct context *) data;
    int32_t printed_ws = print_all_ws_runs(context);

    context->in_ws_run = 0;
    if (CONTEXT_IN_CONTAINER(context)) {
        if (printed_ws > 0) {
            int result;

            /* whitespace was printed from the context */
            if ((result = print_u_literal(SPACE_ALLOWED, term, 5, context)) != CIF_OK) {
                return result;
            }
        } else if (printed_ws == 0) {
            if (u_fprintf(context->out, "\nsave_\n") != 7) {
                return CIF_ERROR;
            } else {
                context->column = 0;
            }
        } else {
            return CIF_ERROR;
        }
    }

    return flush_container(container, context);
}

/*
 * Handles loop_start events by printing a loop header to the output
 * Synthesizes a dummy packet if a flag is set in the context indicating that it should do so.
 */
int handle_loop_start(cif_loop_tp *loop, void *data) {
    static const UChar loop_kw[] = { UCHAR_l, UCHAR_o, UCHAR_o, UCHAR_p, UCHAR_UNDER, 0 };
    static const size_t kw_len = (sizeof(loop_kw) / sizeof(loop_kw[0])) - 1;
    static const UChar placeholder[] = { UCHAR_QUERY, 0 };
    struct context *context = (struct context *) data;

    context->in_ws_run = 0;

    if (!CONTEXT_IN_CONTAINER(context)) {
        return ((print_all_ws_runs(context) < 0) ? CIF_ERROR : CIF_OK);
    } else {
        int result = CIF_OK;
        int32_t nprinted;
        UChar **names;

        context->in_loop = 1;

        /* print the first recorded whitespace run, if any, and the loop_ keyword */
        nprinted = print_ws_run(context);
        if (nprinted > 0) {
            result = print_u_literal(SPACE_ALLOWED, loop_kw, kw_len, context);
        } else if ((nprinted == 0) && (u_fprintf(context->out, "\n%S", loop_kw) == (int32_t) (kw_len + 1))) {
            context->column = kw_len;
        } else {
            result = CIF_ERROR;
        }

        if ((result == CIF_OK) && ((result = cif_loop_get_names(loop, &names)) == CIF_OK)) {
            UChar **name;

            for (name = names; *name; name += 1) {
                if (result == CIF_OK) {

                    /* ensure whitespace separation */
                    nprinted = print_ws_run(context);
                    if ((nprinted == 0) && (u_fputc(UCHAR_LF, context->out) == UCHAR_LF)) {
                        context->column =  0;
                    } else if (nprinted <= 0) {
                        result = CIF_ERROR;
                        free(*name);
                        continue;
                    }

                    /* print the name */
                    result = print_u_literal(SPACE_ALLOWED, *name, u_strlen(*name), context);

                }  /* else don't try to print the name, but we still need to free it */
                free(*name);
            }

            if ((result == CIF_OK) && context->synthesize_packet) {
                context->synthesize_packet = 0;

                /* output a dummy packet for the loop so that it will not be empty */
                if (u_fputc(UCHAR_LF, context->out) != UCHAR_LF) {
                    result = CIF_ERROR;
                } else {
                    context->column = 0;
                    for (name = names; (result == CIF_OK) && *name; name += 1) {
                        /*
                         * *name is an invalid pointer, but we need only to test it against NULL (above), not to
                         * dereference it
                         */
                        if ((result = print_u_literal(SPACE_REQUIRED, placeholder, 1, context)) != CIF_OK) {
                            break;
                        }
                    }

                    if ((result == CIF_OK) && (u_fputc(UCHAR_LF, context->out) != UCHAR_LF)) {
                        result = CIF_ERROR;
                    }
                }
            }

            free(names);
        }

        return result;
    }
}

/*
 * Tracks that the parser has left a loop, and ends the current line if no other whitespace is recorded
 */
int handle_loop_end(cif_loop_tp *loop UNUSED, void *data) {
    struct context *context = (struct context *) data;

    if (CONTEXT_IN_CONTAINER(context)) {
        context->in_loop = 0;

        if (context->column && !context->ws_queue) {
            /* inject synthetic whitespace */
            preserve_whitespace(0, 0, standard_eol_chars, 1, context);
        }
    }

    return CIF_TRAVERSE_CONTINUE;
}

/*
 * Causes each loop packet to start on a new line if no whitespace is obtained from context
 * Intended to handle packet_start events.
 */
int handle_packet_start(cif_packet_tp *packet UNUSED, void *data) {
    struct context *context = (struct context *) data;

    /* no direct whitespace handling at this level */
    if (CONTEXT_IN_CONTAINER(context)) {
        if (context->column && !context->ws_queue) {
            /* inject synthetic whitespace */
            preserve_whitespace(0, 0, standard_eol_chars, 1, context);
        }
    }
    return CIF_TRAVERSE_CONTINUE;
}

/*
 * Suppresses recording looped data
 * Intended to handle packet_end events.
 */
int discard_packet(cif_packet_tp *packet UNUSED, void *context UNUSED) {
    /* no whitespace handling at this point */
    return CIF_TRAVERSE_SKIP_CURRENT;
}

int print_delimited(UChar *str, UChar *delim, int delim_length, UFILE *out) {
    return (u_fprintf(out, "%S%S%S", delim, str, delim) >= delim_length * 2) ? CIF_OK : CIF_ERROR;
}

/*
 * Chooses how much of the given line of text should be included in the next folded segment
 */
ptrdiff_t compute_fold_length(UChar *fold_start, ptrdiff_t line_length, ptrdiff_t target_length, int window,
        int allow_folding_before_semi) {
    assert(target_length > window);

    if (line_length <= target_length + window) {
        /* the line fits without folding */
        return line_length;
    } else {

        /*
         * prefer to fold at a transition from whitespace to non-whitespace, as close as possible to the target length
         */

        int best_category = 0; /* category 0 = no good; 1 = between non-space; 2 = between space; 3 = transition */
        int best_diff = -(window + 1);
        int diff;
        UChar this_char = fold_start[target_length - (window + 1)];
        int is_space = ((this_char == UCHAR_SP) || (this_char == UCHAR_TAB));
        int was_space;

        /* identify the best fold location in the bottom half of the window */
        for (diff = -window; diff; diff += 1) {
            int category;

            was_space = is_space;
            this_char = fold_start[target_length + diff];
            is_space = ((this_char == UCHAR_SP) || (this_char == UCHAR_TAB));

            category = (allow_folding_before_semi || (this_char != UCHAR_SEMI))
                    ? ((was_space * 2) + !is_space)
                    : 0;

            if (category >= best_category) {
                best_diff = diff;
                best_category = category;
            }
        }

        /* look for a better fold location in the top half of the window */
        for (; diff <= window; diff += 1) {
            int category;

            was_space = is_space;
            this_char = fold_start[target_length + diff];
            is_space = ((this_char == UCHAR_SP) || (this_char == UCHAR_TAB));

            category = (allow_folding_before_semi || (this_char != UCHAR_SEMI))
                    ? ((was_space * 2) + !is_space)
                    : 0;

            if (category == 3) {
                /* it doesn't get any better than this */
                best_diff = diff;
                break;
            } else if (category > best_category) {
                best_diff = diff;
                best_category = category;
            } else if ((category == best_category) && (diff <= -best_diff)) {
                best_diff = diff;
                best_category = category;
            }
        }

        if (best_category) {
            /* A viable fold location was found */
            return target_length + best_diff;
        } else {
            /*
             * All characters in the target window are semicolons, and we must not fold before a semicolon.
             * Scan backward in the string to find a viable fold location
             */
            ptrdiff_t best_length;

            for (best_length = target_length - (window + 1); best_length > 0; best_length -= 1) {
                if (fold_start[best_length] != UCHAR_SEMI) {
                    break;
                }
            }

            return best_length;
        }
    }
}

/* prints a string in text-field form, applying line-folding and / or text prefixing as directed */
int print_text_field(UChar *str, int do_fold, int do_prefix, struct context *context) {
    /* CIF line termination characters */
    static const UChar line_term[] = { 0xa, 0xd, 0 };

    int result;

    if (!(do_prefix || do_fold)) {
        result = (u_fprintf(context->out, "\n;%S\n;", str) < 4) ? CIF_ERROR : CIF_OK;
    } else {
        if (u_fprintf(context->out, "\n;%s%s%s", (do_prefix ? PREFIX "\\" : ""), (do_fold ? "\\" : ""),
                ((do_prefix || do_fold) ? "\n" : "")) < 2) {
            result = CIF_ERROR;
        } else {
            UChar *line_start;  /* points to the start of the current logical line */
            UChar *line_end;    /* points to the terminator of the current logical line (line term or string term) */
            int32_t prefix_len = do_prefix ? 2 : 0;

            for (line_start = str; *line_start; line_start = line_end + 1) {
                /* each logical line */
                int32_t line_len = u_strcspn(line_start, line_term);

                line_end = line_start + line_len;

                if (!do_fold) {
                    assert(do_prefix);
                    if (u_fprintf(context->out, PREFIX "%.*S\n", line_len, line_start)
                            < (prefix_len + line_len + 1)) {
                        return CIF_ERROR;
                    }
                } else {
                    UChar *fold_start = line_start;
                    ptrdiff_t fold_len;

                    do {
                        /* each folded segment (even if there's only one; even if it's empty) */
                        ptrdiff_t limit = line_end - fold_start;

                        fold_len = compute_fold_length(fold_start, limit,
                                MAX_FOLD_LENGTH - FOLD_WINDOW - prefix_len , FOLD_WINDOW, do_prefix);
                        assert(0 < fold_len && fold_len <= limit);

                        if (fold_len == limit) {

                            /* whether trailing whitespace or literal backslashes need to be protected */
                            int protect = (fold_len > 0) && (
                                    (fold_start[fold_len - 1] == UCHAR_SP)
                                    || (fold_start[fold_len - 1] == UCHAR_TAB)
                                    || (fold_start[fold_len - 1] == UCHAR_BSL));

                            if (u_fprintf(context->out, "%s%.*S%s\n", (do_prefix ? PREFIX : ""),
                                    fold_len, fold_start, (protect ? "\\\n" : "")) < (prefix_len + fold_len)) {
                                return CIF_ERROR;
                            }
                        } else {
                            if (u_fprintf(context->out, "%s%.*S\\\n", (do_prefix ? PREFIX : ""),
                                    fold_len, fold_start) < (prefix_len + fold_len)) {
                                return CIF_ERROR;
                            }
                        }
                        fold_start += fold_len;
                    } while (fold_start < line_end);
                }

                /* CR/LF line termination provides an extra character to eat */
                if ((*line_end == UCHAR_CR) && (*(line_end + 1) == UCHAR_LF)) {
                    line_end += 1;
                }

                if (!*line_end) {
                    /* lest the loop control expression attempt to dereference an out-of-bounds pointer: */
                    break;
                }
            }

            /* closing delimiter */
            /* the leading newline will already have been output */
            result = (u_fputc(UCHAR_SEMI, context->out) == UCHAR_SEMI) ? CIF_OK : CIF_ERROR;
        }
    }

    if (result == CIF_OK) {
        context->column = 1;
    }

    return result;
}

/*
 * Prints the text of a value to the output
 *
 * ws_needed: indicates whether whitespace is required before the value itself.  If ws_needed is 0 then whitespace is
 * optional, else whitespace is required.  If ws_needed is greater than 1, then all cached whitespace will be printed;
 * otherwise, only the first run, if any, is printed.
 */
int print_value_text(cif_value_tp *value, struct context *context, unsigned int ws_needed) {
    int n_ws;
    UChar *text;
    int result;

    /* output appropriate cached whitespace, if any */
    n_ws = (ws_needed > 1) ? print_all_ws_runs(context) : print_ws_run(context);

    if (n_ws < 0) {
        return CIF_ERROR;
    } else if ((result = cif_value_get_text(value, &text)) == CIF_OK) {
        struct cif_string_analysis_s analysis;

        if ((result = cif_analyze_string(text, !cif_value_is_quoted(value), context->output_format != CIF11,
                MAX_LINE_LENGTH, &analysis)) == CIF_OK) {
            int32_t length;
            int minimum_ws = (n_ws || !ws_needed) ? 0 : 1;

            switch (analysis.delim_length) {
                case 3: /* triple-quoted */
                    if (analysis.num_lines > 1) {
                        if (((result = ensure_space(minimum_ws, analysis.length_first + 3, context)) == CIF_OK)
                                && ((result = print_delimited(text, analysis.delim, 3, context->out))
                                        == CIF_OK)) {
                            context->column = analysis.length_last + 3;
                        }
                        break;
                    } /* else fall through */
                case 1: /* single-quoted */
                case 0: /* whitespace-delimited */
                    length = analysis.length_first + 2 * analysis.delim_length;
                    if (((result = ensure_space(minimum_ws, length, context)) == CIF_OK)
                            && ((result = print_delimited(text, analysis.delim, analysis.delim_length,
                                    context->out)) == CIF_OK)) {
                        context->column = length;
                    }
                    break;
                case 2:
                    /* we don't need to make any further provision for whitespace in this case */
                    return print_text_field(text,
                            /* whether to fold: */
                            (analysis.length_max > MAX_LINE_LENGTH)
                                    || (analysis.length_first >= MAX_LINE_LENGTH)
                                    || analysis.has_reserved_start
                                    || analysis.has_trailing_ws
                                    || (analysis.max_semi_run >= (MAX_FOLD_LENGTH - 1)),
                            /* whether to prefix: */
                            analysis.contains_text_delim
                                    || (analysis.max_semi_run >= (MAX_FOLD_LENGTH - 1)),
                            context
                            );
                default:
                    result = CIF_INTERNAL_ERROR;
                    break;
            }
        }
        free(text);
    }

    return result;
}

/*
 * Prints a List value to the output.  Printing any needed leading whitespace is the responsibility of the caller.
 */
int print_list(cif_value_tp *value, struct context *context) {
    static const UChar list_open[] = { UCHAR_OBRK, 0 };
    static const UChar list_close[] = { UCHAR_CBRK, 0 };
    size_t count;
    int result;

    if (context->output_format == CIF11) {
        /* List values cannot be output in CIF 1.1 format */
        flush_ws(context->ws_queue);
        result = CIF_DISALLOWED_VALUE;
    } else if (((result = cif_value_get_element_count(value, &count)) == CIF_OK)
            && ((result = print_u_literal(SPACE_ALLOWED, list_open, 1, context)) == CIF_OK)) {
        size_t index;

        for (index = 0; index < count; index += 1) {
            cif_value_tp *element;

            if (cif_value_get_element_at(value, index, &element) != CIF_OK) {
                return CIF_INTERNAL_ERROR;
            } else if ((result = print_value(element, context, index && 1)) != CIF_TRAVERSE_CONTINUE) {
                return result;
            }
        }

        if (print_ws_run(context) < 0) {
            result = CIF_ERROR;
        } else {
            result = print_u_literal(SPACE_ALLOWED, list_close, 1, context);
        }
    }

    return result;
}

/*
 * Prints a Table value to the output.  Printing any needed leading whitespace is the responsibility of the caller.
 */
int print_table(cif_value_tp *value, struct context *context) {
    static const UChar table_open[] =  { UCHAR_OBRC, 0 };
    static const UChar table_close[] = { UCHAR_CBRC, 0 };
    static const UChar entry_colon[] = { UCHAR_COLON, 0 };
    const UChar **keys;
    int result;

    if (context->output_format == CIF11) {
        /* Table values cannot be output in CIF 1.1 format */
        flush_ws(context->ws_queue);
        result = CIF_DISALLOWED_VALUE;
    } else if ((result = cif_value_get_keys(value, &keys)) == CIF_OK) {
        if ((result = print_u_literal(SPACE_ALLOWED, table_open, 1, context)) == CIF_OK) {
            const UChar **key;
            int first = 1;

            for (key = keys; *key; key += 1) {
                cif_value_tp *kv;
                cif_value_tp *ev;

                /* key and value are both printed via the machinery for printing values */
                if (((result = cif_value_get_item_by_key(value, *key, &ev)) != CIF_OK)
                        || ((result = cif_value_create(CIF_UNK_KIND, &kv)) != CIF_OK)) {
                    goto cleanup;
                } else {
                    /* copying the key is inefficient, but the original belongs to the table value */
                    if (((result = cif_value_copy_char(kv, *key)) != CIF_OK)
                            || ((result = print_value_text(kv, context, !first)) != CIF_OK)
                            || ((result = print_u_literal(SPACE_FORBIDDEN, entry_colon, 1, context)) != CIF_OK)
                            || ((result = print_value(ev, context, 0)) != CIF_TRAVERSE_CONTINUE)
                            ) {
                        cif_value_free(kv);
                        goto cleanup;
                    }

                    cif_value_free(kv);
                    /* ev (the entry's value) must not be freed -- it belongs to the table */
                }

                first = 0;
            }

            if (print_ws_run(context) < 0) {
                result = CIF_ERROR;
            } else {
                result = print_u_literal(SPACE_ALLOWED, table_close, 1, context);
            }
        }

        cleanup:

        /* free only the key array, not the individual keys, as required by cif_value_get_keys() */
        free(keys);
    }

    return result;
}

/*
 * Prints a value to the output, along with appropriate whitespace.  Whitespace is drawn from that cached in the
 * specified context, or synthesized if necessary.
 *
 * ws_needed_max: If 0, then at most one whitespace run will be printed, and no extra action will be taken if that run
 *      is empty or if there is none available.  If 1, then at most one whitespace run will be printed, and if that run
 *      is empty or there is none available then whitespace will be synthesized.  If greater than 1 then all cached
 *      whitespace will be printed before the value, and if there is none then whitespace will be synthesized.
 */
int print_value(cif_value_tp *value, struct context *context, unsigned int ws_needed_max) {
    static const UChar unk_value_literal[] = { UCHAR_QUERY, 0 };
    static const UChar na_value_literal[] = { UCHAR_DOT, 0 };
    cif_kind_tp kind = cif_value_kind(value);
    int32_t nprinted;

    switch (kind) {
        case CIF_NA_KIND:
        case CIF_UNK_KIND:
            nprinted = ((ws_needed_max > 1) ? print_all_ws_runs(context) : print_ws_run(context));
            if (nprinted < 0) {
                return CIF_ERROR;
            }
            break;
        case CIF_LIST_KIND:
        case CIF_TABLE_KIND:
            nprinted = print_ws_run(context);
            if (ws_needed_max && !nprinted) {
                int result = ensure_space(1, 1, context);

                if (result != CIF_OK) {
                    return result;
                }
            }
            break;
        default:
            nprinted = 0;
            break;
    }

    switch(kind) {
        case CIF_CHAR_KIND:
        case CIF_NUMB_KIND:
            return print_value_text(value, context, ws_needed_max);
        case CIF_NA_KIND:
        case CIF_UNK_KIND:
            return print_u_literal((ws_needed_max && !nprinted),
                    ((kind == CIF_UNK_KIND) ? unk_value_literal : na_value_literal),
                    1, context);
        case CIF_LIST_KIND:
            return print_list(value, context);
        case CIF_TABLE_KIND:
            return print_table(value, context);
        default:
            /* unknown kind */
            return CIF_INTERNAL_ERROR;
    }

    return CIF_ERROR;
}

/* Intended to handle item events. */
int print_item(UChar *name, cif_value_tp *value, void *data) {
    struct context *context = (struct context *) data;
    int32_t nprinted;

    /* name is NULL or the data are outside any container then the value needs to be suppressed */
    if (!(CONTEXT_IN_CONTAINER(context) && name)) {
        /*
         * Neither the item / value nor any internal insignificant whitespace should be printed.  If there is cached
         * whitespace, however, then whatever of it appears at top level is set up to be merged with whatever
         * whitespace is reported next.
         */
        struct ws_node *ws_start = context->ws_queue;

        if (ws_start) {
            /* discard all whitespace runs but the first one (if in a loop) or up to two (otherwise) */
            struct ws_node *last_run = context->in_loop ? ws_start : ws_start->next_run;

            if (last_run) {
                flush_ws(last_run->next_run);
                last_run->next_run = NULL;

                if (last_run != ws_start) {
                    /* last_run points to the second run */
                    /* merge whitespace runs */
                    struct ws_node *this_piece = ws_start;

                    while (this_piece->next_piece) {
                        this_piece = this_piece->next_piece;
                    }

                    this_piece->next_piece = last_run;
                    ws_start->next_run = NULL;
                }
            }

            /* pretend the whitespace was unbroken by any value or item */
            context->in_ws_run = 1;
        }

        return CIF_TRAVERSE_CONTINUE;
    } else {
        /*
         * Marking the end of the whitespace run only _inside_ this conditional scope helps whitespace runs around and
         * inside skipped values to be merged together.
         */
        context->in_ws_run = 0;

        if (!context->in_loop) {
            /* write the data name, with appropriate whitespace separation */

            nprinted = print_ws_run(context);

            if (nprinted > 0) {
                int result;

                if ((result = print_u_literal(SPACE_ALLOWED, name, u_strlen(name), context)) != CIF_OK) {
                    return result;
                }
            } else if (nprinted == 0) {
                int32_t n_printed = u_fprintf(context->out, "\n%S", name);

                if (n_printed < 3) {
                    return CIF_ERROR;
                } else {
                    context->column = n_printed - 1;
                }
            } else {
                return CIF_ERROR;
            }
        }

        /* write the value */
        return print_value(value, context, 2);
    }
}

int error_callback(int code, size_t line, size_t column, const UChar *text, size_t length, void *data) {
    struct context *context = (struct context *) data;

    context->error_count += 1;

    if (!context->quiet) {
        if ((0 <= code) && (code <= cif_nerr)) {
            u_fprintf(context->ustderr, "CIF error %d at line %lu, column %lu, (near '%*S'): %s\n", code,
                    (uint32_t) line, (uint32_t) column, (int32_t) length, text, cif_errlist[code]);
        } else {
            u_fprintf(context->ustderr, "CIF error %d at line %lu, column %lu, (near '%*S'): (unknown error code)\n",
                    code, (uint32_t) line, (uint32_t) column, (int32_t) length, text);
        }
    }

    if (context->halt_on_error) {
        return code;
    } else {
        /*
         * Whitespace handling is on one hand sufficiently self-correcting, and on the other hand sufficiently tricky,
         * that we do not need or want to apply corrective action with respect to whitespace for any documented error
         * that the parser might raise.
         *
         * We take corrective structural action only for one error code, as the parser's recovery behavior in that case
         * would otherwise lead to an (also) invalid CIF being produced.
         */
        if (code == CIF_EMPTY_LOOP) {
            context->synthesize_packet = 1;
        }

        return CIF_OK;
    }
}

/*
 * Translates the specified 'extra' end-of-line characters to to newlines and the specified 'extra' whitespace
 * characters to spaces in the provided text buffer.  The buffer is expected to be nul-terminated, but if its length is
 * already known then that can be specified as an argument for a minor efficiency improvement.  Otherwise, the length
 * should be passed as ((size_t) -1).
 */
void translate_whitespace(UChar *text, size_t length, const UChar *extra_eol, const UChar *extra_ws) {
    if (length == (size_t) -1) {
        length = u_strlen(text);
    }
    if (extra_eol && extra_eol[0]) {
        size_t span_start = 0;

        for (;;) {
            span_start += u_strcspn(text + span_start, extra_eol);
            if (span_start >= length) {
                break;
            } else {
                text[span_start++] = UCHAR_LF;
            }
        }
    }

    if (extra_ws && extra_ws[0]) {
        size_t span_start = 0;

        for (;;) {
            span_start += u_strcspn(text + span_start, extra_ws);
            if (span_start >= length) {
                break;
            } else {
                text[span_start++] = UCHAR_SP;
            }
        }
    }
}

/*
 * A callback function by which whitespace (including comments) in the input CIF can be handled.  This version
 * accumulates whitespace segments in a linked list of linked lists, for the output routines to use later.
 *
 * Non-standard whitespace characters are translated to standard ones here.
 */
void preserve_whitespace(size_t line UNUSED, size_t column UNUSED, const UChar *ws, size_t length, void *data) {
    struct context *context = (struct context *) data;

    if (!ws) {
        fprintf(stderr, "warning: received a null whitespace segment");
    } else {
        struct ws_node *ws_node = (struct ws_node *) malloc(sizeof(*ws_node));

        if (!ws_node) {
            perror(context->progname);
            /* this is not, in itself, a fatal condition */
        } else {
            /*
             * NOTE: the ws argument might not be null terminated!
             */
            ws_node->next_piece = NULL;
            ws_node->next_run = NULL;
            ws_node->ws = (UChar *) malloc((length + 1) * sizeof(UChar));

            if (!ws_node->ws) {
                free(ws_node);
                perror(context->progname);
                /* this is not, in itself, a fatal condition */
            } else {
                struct ws_node *ws_cache = context->ws_queue;

                ws_node->ws[length] = 0;
                if (length) {
                    memcpy(ws_node->ws, ws, length * sizeof(UChar));
                    translate_whitespace(ws_node->ws, length, context->extra_eol, context->extra_ws);
                }

                if (!ws_cache) {
                    context->ws_queue = ws_node;
                } else {
                    while (ws_cache->next_run) {
                        ws_cache = ws_cache->next_run;
                    }

                    if (context->in_ws_run) {
                        while (ws_cache->next_piece) {
                            ws_cache = ws_cache->next_piece;
                        }
                        ws_cache->next_piece = ws_node;
                    } else {
                        ws_cache->next_run = ws_node;
                    }
                }

                /*
                 * zero-length whitespace signals the end of a whitespace run, including one that could or should have
                 * been present, but wasn't.
                 */
                context->in_ws_run = (length != 0);
            }
        }
    }
}

/*
 * A program to convert among various dialects of CIF
 */
int main(int argc, char *argv[]) {
    struct cif_parse_opts_s *parse_opts;
    struct context context = { 0 };
    struct cif_handler_s handler = {
        print_header,      /* cif start */
        handle_cif_end,    /* cif end */
        open_block,        /* block start */
        flush_container,   /* block end */
        open_frame,        /* frame start */
        finish_frame,      /* frame end */
        handle_loop_start, /* loop_start */
        handle_loop_end,   /* loop end */
        handle_packet_start, /* packet start */
        discard_packet,    /* packet end */
        print_item         /* item */
    };
    cif_tp *cif = NULL;
    int result;
    int ignored;

    if (cif_parse_options_create(&parse_opts) != CIF_OK) {
        exit(2);
    }

    parse_opts->handler = &handler;
    parse_opts->user_data = &context;
    process_args(argc, argv, parse_opts);
    assert(context.out && context.ustderr);

    result = cif_parse(stdin, parse_opts, &cif);

    if (result != CIF_OK) {
        handle_cif_end(cif, &context);
    }
    ignored = cif_destroy(cif);

    /* clean up */
    free(parse_opts);
    if (context.extra_ws) {
        free(context.extra_ws);
    }
    if (context.extra_eol) {
        free(context.extra_eol);
    }

    /*
     * The handler already closed the Unicode output and error streams
     */

    /*
     * Exit codes:
     *  3 - parse aborted because of errors
     *  2 - parse skipped (e.g. from function usage() calling exit())
     *  1 - parse completed with errors
     *  0 - parse completed without errors
     */
    return (result != CIF_OK) ? 3 : (context.error_count ? 1 : 0);
}
