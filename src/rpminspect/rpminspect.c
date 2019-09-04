/*
 * Copyright (C) 2019  Red Hat, Inc.
 * Author(s):  David Cantrell <dcantrell@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <glob.h>

#include "rpminspect.h"
#include "builds.h"

/* Global librpminspect state */
struct rpminspect ri;

static void usage(const char *progname) {
    assert(progname != NULL);

    printf("Compare package builds for policy compliance and consistency.\n\n");
    printf("Usage: %s [OPTIONS] [before build] [after build]\n", progname);
    printf("Options:\n");
    printf("  -c FILE, --config=FILE   Configuration file to use\n");
    printf("                             (default: %s)\n", CFGFILE);
    printf("  -T LIST, --tests=LIST    List of tests to run\n");
    printf("                             (default: ALL)\n");
    printf("  -E LIST, --exclude=LIST  List of tests to exclude\n");
    printf("                             (default: none)\n");
    printf("  -a LIST, --arches=LIST   List of architectures to check\n");
    printf("  -r STR, --release=STR    Product release string\n");
    printf("  -o FILE, --output=FILE   Write results to FILE\n");
    printf("                             (default: stdout)\n");
    printf("  -F TYPE, --format=TYPE   Format output results as TYPE\n");
    printf("                             (default: text)\n");
    printf("  -l, --list               List available tests and formats\n");
    printf("  -w PATH, --workdir=PATH  Temporary directory to use\n");
    printf("                             (default: %s)\n", DEFAULT_WORKDIR);
    printf("  -f, --fetch-only         Fetch builds only, do not perform inspections\n");
    printf("                             (implies -k)\n");
    printf("  -k, --keep               Do not remove the comparison working files\n");
    printf("  -v, --verbose            Verbose inspection output\n");
    printf("                           when finished, display full path\n");
    printf("  -?, --help               Display usage information\n");
    printf("  -V, --version            Display program version\n");
    printf("\nSee the rpminspect(1) man page for more information.\n");

    return;
}

/*
 * Get the product release string by grabbing a possible dist tag from
 * the Release value.  Dist tags begin with '.' and go to the end of the
 * Release value.  Trim any trailing '/' characters in case the user is
 * specifying a build from a local path.
 */
static char *get_product_release(const char *before, const char *after) {
    char *pos = NULL;
    char *before_product = NULL;
    char *after_product = NULL;

    assert(after != NULL);

    pos = rindex(after, '.');
    if (!pos) {
        fprintf(stderr, "*** Product release for after build (%s) is empty\n", after);
        return NULL;
    }

    /*
     * Get the character after the last occurrence of a period. This should
     * tell us what release flag the product is.
     */
    pos += 1;
    after_product = strdup(pos);
    if (!after_product) {
        fprintf(stderr, "*** Product release for after build (%s) is empty\n", after);
        return NULL;
    }

    /*
     * Trim any trailing slashes in case the user is specifying builds from
     * local paths
     */
    after_product[strcspn(after_product, "/")] = 0;

    if (before) {
        pos = rindex(before, '.') + 1;
        before_product = strdup(pos);

        if (!before_product) {
            fprintf(stderr, "*** Product release for before build (%s) is empty\n", before);
            free(after_product);
            return NULL;
        }

        /*
         * Trim any trailing slashes in case the user is specifying builds from
         * local paths
         */
        before_product[strcspn(before_product, "/")] = 0;

        if (strcmp(before_product, after_product)) {
            fprintf(stderr, "*** Builds have different product releases (%s != %s)\n", before_product, after_product);
            free(before_product);
            free(after_product);
            return NULL;
        }

        free(before_product);
    }

    return after_product;
}

/*
 * Used to ensure the user only specifies the -T or -E option.
 */
static void check_inspection_options(const bool inspection_opt, const char *progname)
{
    assert(progname != NULL);

    if (inspection_opt) {
        fprintf(stderr, "*** The -T and -E options are mutually exclusive\n");
        fprintf(stderr, "*** See `%s --help` for more information.\n", progname);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

    return;
}

/*
 * Used in the -T and -E option processing to report any unknown
 * test names provided.  Exit if true.
 */
static void check_found(const bool found, const char *inspection, const char *progname)
{
    assert(inspection != NULL);
    assert(progname != NULL);

    if (!found) {
        fprintf(stderr, "*** Unknown test specified: `%s`\n", inspection);
        fprintf(stderr, "*** See `%s --help` for more information.\n", progname);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

    return;
}

/*
 * Used in the -T and -E option processing to handle each test
 * flag.  Arguments:
 *     inspection      The name of the inspection from the command
 *                     line.  e.g., "-T license,manpage" would make
 *                     two calls to this function with inspection
 *                     being "license" and then "manpage".
 *     exclude         True if -E option, False otherwise.
 *     selected        The selected test bitmap from the caller.
 * Returns true if the inspection name is valid.
 */
static bool process_inspection_flag(const char *inspection, const bool exclude, uint64_t *selected)
{
    int i = 0;
    bool found = false;

    assert(inspection != NULL);
    assert(selected != NULL);

    if (!strcasecmp(inspection, "ALL")) {
        /* ALL tests specified */
        if (exclude) {
            *selected = 0;
            return true;
        } else {
            *selected = ~0;
            return true;
        }
    }

    for (i = 0; inspections[i].flag != 0; i++) {
        if (!strcasecmp(inspection, inspections[i].name)) {
            /* user specified a valid inspection */
            if (exclude) {
                *selected &= ~(inspections[i].flag);
                found = true;
                break;
            } else {
                *selected |= inspections[i].flag;
                found = true;
                break;
            }
        }
    }

    return found;
}

int main(int argc, char **argv) {
    char *progname = basename(argv[0]);
    int c, i, j;
    int idx = 0;
    int ret = EXIT_SUCCESS;
    glob_t expand;
    char *short_options = "c:T:E:a:r:o:F:lw:fkv\?V";
    struct option long_options[] = {
        { "config", required_argument, 0, 'c' },
        { "tests", required_argument, 0, 'T' },
        { "exclude", required_argument, 0, 'E' },
        { "arches", required_argument, 0, 'a' },
        { "release", required_argument, 0, 'r' },
        { "list", no_argument, 0, 'l' },
        { "output", required_argument, 0, 'o' },
        { "format", required_argument, 0, 'F' },
        { "workdir", required_argument, 0, 'w' },
        { "fetch-only", no_argument, 0, 'f' },
        { "keep", no_argument, 0, 'k' },
        { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, '?' },
        { "version", no_argument, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    char *cfgfile = NULL;
    char *archopt = NULL;
    char *walk = NULL;
    char *token = NULL;
    char *workdir = NULL;
    char *output = NULL;
    char *release = NULL;
    int formatidx = -1;
    bool fetch_only = false;
    bool keep = false;
    bool verbose = false;
    int mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    bool found = false;
    char *inspection = NULL;
    uint64_t selected = 0;
    bool inspection_opt = false;
    bool exclude = false;
    size_t width = tty_width();
    string_list_t *valid_arches = NULL;
    string_entry_t *arch = NULL;
    string_entry_t *entry = NULL;

    /* parse command line options */
    while (1) {
        c = getopt_long(argc, argv, short_options, long_options, &idx);

        if (c == -1) {
            break;
        }

        switch (c) {
            case 'c':
                /* Capture user specified config file */
                cfgfile = strdup(optarg);
                break;
            case 'T':
            case 'E':
                /* Process the -T or the -E options */
                check_inspection_options(inspection_opt, progname);

                if (c == 'T') {
                    selected = 0;
                    exclude = false;
                } else {
                    selected = ~0;
                    exclude = true;
                }

                while ((inspection = strsep(&optarg, ",")) != NULL) {
                    found = process_inspection_flag(inspection, exclude, &selected);
                    check_found(found, inspection, progname);
                }

                inspection_opt = true;
                break;
            case 'a':
                archopt = strdup(optarg);
                break;
            case 'r':
                release = strdup(optarg);
                break;
            case 'o':
                output = strdup(optarg);
                break;
            case 'F':
                /* validate the specified output format */
                for (i = 0; formats[i].type != -1; i++) {
                    if (!strcasecmp(formats[i].name, optarg)) {
                        formatidx = formats[i].type;
                        break;
                    }
                }

                if (formatidx < 0) {
                    fprintf(stderr, "*** Invalid output format: `%s`.\n", optarg);
                    fflush(stderr);
                    return EXIT_FAILURE;
                }

                break;
            case 'l':
                /* list the formats available */
                printf("Available output formats:\n");

                for (i = 0; formats[i].type != -1; i++) {
                    if (i > 0) {
                        printf("\n");
                    }

                    printf("    %s\n", formats[i].name);

                    if (formats[i].desc != NULL) {
                        printwrap(formats[i].desc, width, 8, stdout);
                        printf("\n");
                    }
                }

                /* list the inspections available */
                printf("\nAvailable inspections:\n");

                for (i = 0; inspections[i].flag != 0; i++) {
                    if (i > 0) {
                        printf("\n");
                    }

                    printf("    %s\n", inspections[i].name);

                    if (inspections[i].desc != NULL) {
                        printwrap(inspections[i].desc, width, 8, stdout);
                        printf("\n");
                    }
                }

                exit(EXIT_SUCCESS);
                break;
            case 'w':
                if (index(optarg, '~')) {
                    /* allow for ~ expansion */
                    j = glob(optarg, GLOB_TILDE_CHECK, NULL, &expand);

                    if (j == 0 && expand.gl_pathc == 1) {
                        workdir = strdup(expand.gl_pathv[0]);
                    } else {
                        fprintf(stderr, "*** Unable to expand workdir: `%s`: %s", optarg, strerror(errno));
                        fflush(stderr);
                        return EXIT_FAILURE;
                    }

                    globfree(&expand);
                } else {
                    workdir = strdup(optarg);
                }

                break;
            case 'f':
                fetch_only = true;        /* fall-thru: -f implies -k */
                __attribute__ ((fallthrough));
            case 'k':
                keep = true;
                break;
            case 'v':
                verbose = true;
                break;
            case '?':
                usage(progname);
                exit(0);
            case 'V':
                printf("%s version %s\n", progname, PACKAGE_VERSION);
                exit(0);
            default:
                fprintf(stderr, "?? getopt returned character code 0%o ??\n", c);
                fflush(stderr);
                exit(EXIT_FAILURE);
        }
    }

    /*
     * Find an appropriate configuration file. This involves:
     *
     *  - Using the user-passed value and sanity-checking it,
     *  - Using the global default if it exists, or
     *  - Telling the user they need to install a required dependency.
     */
    if (cfgfile != NULL && access(cfgfile, F_OK|R_OK) == -1) {
        fprintf(stderr, "Specified config file (%s) is unreadable.\n", cfgfile);
        exit(EXIT_FAILURE);
    } else if (cfgfile == NULL && access(CFGFILE, F_OK|R_OK) == 0) {
        cfgfile = strdup(CFGFILE);
    } else if (cfgfile == NULL) {
        fprintf(stderr, "Unable to read the default config file (%s).\n", CFGFILE);
        fprintf(stderr, "Have you installed an rpminspect-data package for your distro?\n");
        exit(EXIT_FAILURE);
    }

    /* Initialize librpminspect */
    if (init_rpminspect(&ri, cfgfile) != 0) {
        fprintf(stderr, "Failed to read configuration file\n");
        exit(EXIT_FAILURE);
    }

    free(cfgfile);

    /* various options from the command line */
    ri.verbose = verbose;
    ri.product_release = release;

    /* Copy in user-selected tests if they specified something */
    if (selected != 0 || exclude) {
        ri.tests = selected;
    }

    /* the user did not specify a working directory */
    if (workdir != NULL) {
        free(ri.workdir);
        ri.workdir = workdir;
    }

    /*
     * we should exactly one more argument (single build) or two arguments
     * (a before and after build)
     */
    if (optind == (argc - 1)) {
        /* only a single build specified */
        ri.after = strdup(argv[optind]);
    } else if ((optind + 1) == (argc - 1)) {
        /* we got a before and after build */
        ri.before = strdup(argv[optind]);
        ri.after = strdup(argv[optind + 1]);
    } else {
        /* user gave us too many arguments */
        fprintf(stderr, "*** Invalid before and after build specification.\n");
        fprintf(stderr, "*** See `%s --help` for more information.\n", progname);
        fflush(stderr);
        free_rpminspect(&ri);
        return EXIT_FAILURE;
    }

    /*
     * Fetch-only mode can only work with a single build
     */
    if (fetch_only && ri.before) {
        fprintf(stderr, "*** Fetch only mode takes a single build specification.\n");
        fprintf(stderr, "*** See `%s --help` for more information.\n", progname);
        fflush(stderr);
        free_rpminspect(&ri);
        return EXIT_FAILURE;
    }

    /*
     * Determine product release unless the user specified one.
     */
    if (ri.product_release == NULL) {
        ri.product_release = get_product_release(ri.before, ri.after);

        if (ri.product_release == NULL) {
            fflush(stderr);
            free_rpminspect(&ri);
            return EXIT_FAILURE;
        }
    }

    /* initialize librpm, we'll be using it */
    if (init_librpm() != RPMRC_OK) {
        fprintf(stderr, "*** unable to read RPM configuration\n");
        fflush(stderr);
        return EXIT_FAILURE;
    }

    /* if an architecture list is specified, validate it */
    if (archopt) {
        /* initialize the list of allowed architectures */
        ri.arches = calloc(1, sizeof(*ri.arches));
        assert(ri.arches != NULL);
        TAILQ_INIT(ri.arches);

        /* get a list of valid architectures */
        valid_arches = get_all_arches(&ri);

        /* collect the specified architectures */
        walk = archopt;

        while ((token = strsep(&walk, ",")) != NULL) {
            /* if the architecture is invalid, report it and exit */
            found = false;

            TAILQ_FOREACH(arch, valid_arches, items) {
                if (!strcmp(token, arch->data)) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                fprintf(stderr, "*** Unsupported architecture specified: `%s`\n", token);
                fprintf(stderr, "*** See `%s --help` for more information.\n", progname);
                fflush(stderr);
                return EXIT_FAILURE;
            }

            /* architecture is valid, save it */
            entry = calloc(1, sizeof(*entry));
            assert(entry != NULL);

            entry->data = strdup(token);
            assert(entry->data != NULL);

            TAILQ_INSERT_TAIL(ri.arches, entry, items);
        }

        /* clean up */
        free(archopt);
        list_free(valid_arches, free);
    }

    /* create the working directory */
    if (mkdirp(ri.workdir, mode)) {
        fprintf(stderr, "*** Unable to create directory %s: %s\n", ri.workdir, strerror(errno));
        fflush(stderr);
        free_rpminspect(&ri);
        return EXIT_FAILURE;
    }

    /* validate and gather the builds specified */
    if ((ret = gather_builds(&ri, fetch_only))) {
        fprintf(stderr, "*** Failed to gather specified builds.\n");
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

    /* perform the selected inspections */
    if (!fetch_only) {
        for (i = 0; inspections[i].flag != 0; i++) {
            /* test not selected by user */
            if (!(ri.tests & inspections[i].flag)) {
                continue;
            }

           /* inspection requires before/after builds and we have one */
           if (ri.before == NULL && !inspections[i].single_build) {
               continue;
           }

            if (!inspections[i].driver(&ri)) {
                ret = EXIT_FAILURE;
            }
        }

        /* output the results */
        if (formatidx == -1) {
            formatidx = 0;                 /* default to 'text' output */
        }

        if (ri.results != NULL) {
            formats[formatidx].driver(ri.results, output);
        }
    }

    /* Clean up */
    if (keep) {
        printf("\nKeeping working directory: %s\n", ri.worksubdir);
    } else {
        if (rmtree(ri.workdir, true, true)) {
           fprintf(stderr, "*** Error removing directory %s: %s\n", ri.workdir, strerror(errno));
           fflush(stderr);
        }
    }

    free_rpminspect(&ri);

    return EXIT_SUCCESS;
}
