/*
 * brscan - scan from a Brother DCP-7030 on the command line.
 */
#include "../core/brscan.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static volatile int cancel_flag = 0;

static void on_sigint(int sig)
{
    (void)sig;
    cancel_flag = 1;
}

static void on_progress(double f, void *ctx)
{
    (void)ctx;
    if (f >= 0)
        fprintf(stderr, "\rScanning... %3d%%", (int)(f * 100));
    else
        fprintf(stderr, "\rScanning...");
    fflush(stderr);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: brscan -o FILE [options]\n"
            "       brscan --probe\n"
            "\n"
            "  -o, --output FILE     output file (.pdf, .png, .jpg)\n"
            "  -r, --res DPI         100, 150, 200, 300 (default), 600\n"
            "  -m, --mode MODE       color (default), gray, bw\n"
            "  -f, --format FMT      pdf, png, jpg (default: from file extension)\n"
            "      --width MM        scan width in mm (default 210)\n"
            "      --height MM       scan height in mm (default 295)\n"
            "      --rotate          rotate the result 180 degrees\n"
            "      --probe           check the scanner responds, then exit\n"
            "      --dump FILE       save the raw USB scan stream (debugging)\n"
            "  -v, --verbose         log USB traffic to stderr\n");
}

static int parse_format(const char *s, brscan_format *fmt)
{
    if (!strcasecmp(s, "pdf")) *fmt = BRSCAN_FORMAT_PDF;
    else if (!strcasecmp(s, "png")) *fmt = BRSCAN_FORMAT_PNG;
    else if (!strcasecmp(s, "jpg") || !strcasecmp(s, "jpeg")) *fmt = BRSCAN_FORMAT_JPEG;
    else return -1;
    return 0;
}

int main(int argc, char **argv)
{
    brscan_options opts;
    brscan_default_options(&opts);
    const char *output = NULL;
    const char *format = NULL;
    int probe = 0;
    int rotate = 0;

    enum { OPT_WIDTH = 256, OPT_HEIGHT, OPT_ROTATE, OPT_PROBE, OPT_DUMP };
    static const struct option longopts[] = {
        { "output",  required_argument, NULL, 'o' },
        { "res",     required_argument, NULL, 'r' },
        { "mode",    required_argument, NULL, 'm' },
        { "format",  required_argument, NULL, 'f' },
        { "width",   required_argument, NULL, OPT_WIDTH },
        { "height",  required_argument, NULL, OPT_HEIGHT },
        { "rotate",  no_argument,       NULL, OPT_ROTATE },
        { "probe",   no_argument,       NULL, OPT_PROBE },
        { "dump",    required_argument, NULL, OPT_DUMP },
        { "verbose", no_argument,       NULL, 'v' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int c;
    while ((c = getopt_long(argc, argv, "o:r:m:f:vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'o': output = optarg; break;
        case 'r': opts.dpi = atoi(optarg); break;
        case 'm':
            if (!strcasecmp(optarg, "color") || !strcasecmp(optarg, "colour"))
                opts.mode = BRSCAN_MODE_COLOR;
            else if (!strcasecmp(optarg, "gray") || !strcasecmp(optarg, "grey"))
                opts.mode = BRSCAN_MODE_GRAY;
            else if (!strcasecmp(optarg, "bw"))
                opts.mode = BRSCAN_MODE_BW;
            else {
                fprintf(stderr, "brscan: unknown mode '%s'\n", optarg);
                return 2;
            }
            break;
        case 'f': format = optarg; break;
        case OPT_WIDTH: opts.width_mm = atof(optarg); break;
        case OPT_HEIGHT: opts.height_mm = atof(optarg); break;
        case OPT_ROTATE: rotate = 1; break;
        case OPT_PROBE: probe = 1; break;
        case OPT_DUMP: brscan_set_dump_path(optarg); break;
        case 'v': brscan_set_verbose(1); break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }

    if (probe) {
        char report[4096];
        brscan_status st = brscan_probe(report, sizeof(report));
        if (st != BRSCAN_OK) {
            fprintf(stderr, "brscan: %s\n", brscan_strerror(st));
            return 1;
        }
        printf("Scanner OK. %s\n", report);
        return 0;
    }

    if (!output) {
        usage();
        return 2;
    }

    brscan_format fmt = BRSCAN_FORMAT_PDF;
    if (!format) {
        const char *dot = strrchr(output, '.');
        format = dot ? dot + 1 : "pdf";
    }
    if (parse_format(format, &fmt)) {
        fprintf(stderr, "brscan: unknown format '%s'\n", format);
        return 2;
    }

    signal(SIGINT, on_sigint);

    brscan_image img;
    brscan_status st = brscan_scan(&opts, &img, on_progress, NULL, &cancel_flag);
    fputc('\n', stderr);
    if (st != BRSCAN_OK) {
        fprintf(stderr, "brscan: %s\n", brscan_strerror(st));
        return 1;
    }

    if (rotate)
        brscan_image_rotate180(&img);
    st = brscan_write_file(&img, output, fmt);
    if (st != BRSCAN_OK) {
        fprintf(stderr, "brscan: %s\n", brscan_strerror(st));
        brscan_image_free(&img);
        return 1;
    }
    fprintf(stderr, "Saved %s (%dx%d, %d dpi)\n", output, img.width, img.height, img.dpi);
    brscan_image_free(&img);
    return 0;
}
