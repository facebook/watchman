/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <getopt.h>

#define IS_REQUIRED(x)  (x) == REQ_STRING

/* One does not simply use getopt_long() */

void usage(struct watchman_getopt *opts, FILE *where)
{
  int i;
  size_t len;
  size_t longest = 0;
  const char *label;

  fprintf(where, "Usage: watchman [opts] command\n");

  /* measure up option names so we can format nicely */
  for (i = 0; opts[i].optname; i++) {
    label = opts[i].arglabel ? opts[i].arglabel : "ARG";

    len = strlen(opts[i].optname);
    switch (opts[i].argtype) {
      case REQ_STRING:
        len += strlen(label) + strlen("=");
        break;
      default:
        ;
    }

    if (opts[i].shortopt) {
      len += strlen("-X, ");
    }

    if (len > longest) {
      longest = len;
    }
  }

  /* space between option definition and help text */
  longest += 3;

  for (i = 0; opts[i].optname; i++) {
    char buf[80];

    if (!opts[i].helptext) {
      // This is a signal that this option shouldn't be printed out.
      continue;
    }

    label = opts[i].arglabel ? opts[i].arglabel : "ARG";

    fprintf(where, "\n ");
    if (opts[i].shortopt) {
      fprintf(where, "-%c, ", opts[i].shortopt);
    } else {
      fprintf(where, "    ");
    }
    switch (opts[i].argtype) {
      case REQ_STRING:
        snprintf(buf, sizeof(buf), "--%s=%s", opts[i].optname, label);
        break;
      default:
        snprintf(buf, sizeof(buf), "--%s", opts[i].optname);
        break;
    }

    fprintf(where, "%-*s ", (unsigned int)longest, buf);

    fprintf(where, "%s", opts[i].helptext);
    fprintf(where, "\n");
  }

  print_command_list_for_help(where);

  fprintf(
      where,
      "\n"
      "See https://github.com/facebook/watchman#watchman for more help\n"
      "\n"
      "Watchman, by Wez Furlong.\n"
      "Copyright 2012-2017 Facebook, Inc.\n");

  exit(1);
}

bool w_getopt(struct watchman_getopt *opts, int *argcp, char ***argvp,
    char ***daemon_argvp)
{
  int num_opts, i;
  char *nextshort;
  int argc = *argcp;
  char **argv = *argvp;
  int long_pos = -1;
  int res;
  int num_daemon = 0;

  /* first build up the getopt_long bits that we need */
  for (num_opts = 0; opts[num_opts].optname; num_opts++) {
    ;
  }

  /* to hold the args we pass to the daemon */
  auto daemon_argv = (char**)calloc(num_opts + 1, sizeof(char*));
  if (!daemon_argv) {
    perror("calloc daemon opts");
    abort();
  }
  *daemon_argvp = daemon_argv;

  /* something to hold the long options */
  auto long_opts = (option*)calloc(num_opts + 1, sizeof(struct option));
  if (!long_opts) {
    perror("calloc struct option");
    abort();
  }

  /* and the short options */
  auto shortopts = (char*)malloc((1 + num_opts) * 2);
  if (!shortopts) {
    perror("malloc shortopts");
    abort();
  }
  nextshort = shortopts;
  nextshort[0] = ':';
  nextshort++;

  /* now transfer information into the space we made */
  for (i = 0; i < num_opts; i++) {
    long_opts[i].name = (char*)opts[i].optname;
    long_opts[i].val = opts[i].shortopt;
    switch (opts[i].argtype) {
      case OPT_NONE:
        long_opts[i].has_arg = no_argument;
        break;
      case REQ_STRING:
      case REQ_INT:
        long_opts[i].has_arg = required_argument;
        break;
    }

    if (opts[i].shortopt) {
      nextshort[0] = (char)opts[i].shortopt;
      nextshort++;

      if (long_opts[i].has_arg != no_argument) {
        nextshort[0] = ':';
        nextshort++;
      }
    }
  }

  nextshort[0] = 0;

  while ((res = getopt_long(argc, argv, shortopts,
        long_opts, &long_pos)) != -1) {
    struct watchman_getopt *o;

    switch (res) {
      case ':':
        /* missing option argument.
         * Check to see if it was actually optional */
        for (long_pos = 0; long_pos < num_opts; long_pos++) {
          if (opts[long_pos].shortopt == optopt) {
            if (IS_REQUIRED(opts[long_pos].argtype)) {
              fprintf(stderr, "--%s (-%c) requires an argument",
                  opts[long_pos].optname,
                  opts[long_pos].shortopt);
              return false;
            }
          }
        }
        break;

      case '?':
        /* unknown option */
        fprintf(stderr, "Unknown or invalid option! %s\n", argv[optind-1]);
        usage(opts, stderr);
        return false;

      default:
        if (res == 0) {
          /* we got a long option */
          o = &opts[long_pos];
        } else {
          /* map short option to the real thing */
          o = NULL;
          for (long_pos = 0; long_pos < num_opts; long_pos++) {
            if (opts[long_pos].shortopt == res) {
              o = &opts[long_pos];
              break;
            }
          }
        }

        if (o->is_daemon) {
          char *val;
          ignore_result(asprintf(&val, "--%s=%s", o->optname, optarg));
          daemon_argv[num_daemon++] = val;
        }

        /* store the argument if we found one */
        if (o->argtype != OPT_NONE && o->val && optarg) {
          switch (o->argtype) {
            case REQ_INT:
            {
              auto ival = json_integer(atoi(optarg));
              *(int*)o->val = (int)json_integer_value(ival);
              cfg_set_arg(o->optname, ival);
              break;
            }
            case REQ_STRING:
            {
              auto sval = typed_string_to_json(optarg, W_STRING_UNICODE);
              *(char**)o->val = strdup(optarg);
              cfg_set_arg(o->optname, sval);
              break;
            }
            case OPT_NONE:
              ;
          }
        }
        if (o->argtype == OPT_NONE && o->val) {
          auto bval = json_true();
          *(int*)o->val = 1;
          cfg_set_arg(o->optname, bval);
        }
    }

    long_pos = -1;
  }

  free(long_opts);
  free(shortopts);

  *argcp = argc - optind;
  *argvp = argv + optind;
  return true;
}

/* vim:ts=2:sw=2:et:
 */
