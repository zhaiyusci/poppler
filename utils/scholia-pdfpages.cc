//========================================================================
//
// scholia-pdfpages.cc
//
// This file is licensed under the GPLv2 or later
//
//========================================================================

#include "config.h"
#include <poppler-config.h>

#include <cstdio>

#include "ScholiaPdfPages.h"
#include "parseargs.h"

static int insertBlankAfter = -1;
static int deletePage = -1;
static bool printVersion = false;
static bool printHelp = false;

static const ArgDesc argDesc[] = { { "-insert-blank-after", argInt, &insertBlankAfter, 0, "insert one blank page after the given 1-based page number; use 0 to insert before page 1" },
                                   { "-delete-page", argInt, &deletePage, 0, "delete the given 1-based page number" },
                                   { "-v", argFlag, &printVersion, 0, "print copyright and version info" },
                                   { "-h", argFlag, &printHelp, 0, "print usage information" },
                                   { "-help", argFlag, &printHelp, 0, "print usage information" },
                                   { "--help", argFlag, &printHelp, 0, "print usage information" },
                                   { "-?", argFlag, &printHelp, 0, "print usage information" },
                                   {} };

static constexpr int kOtherError = 99;

int main(int argc, char *argv[])
{
    const bool parseOK = parseArgs(argDesc, &argc, argv);
    const bool wantsInsert = insertBlankAfter >= 0;
    const bool wantsDelete = deletePage >= 0;
    if (!parseOK || argc != 3 || printVersion || printHelp || wantsInsert == wantsDelete) {
        fprintf(stderr, "scholia-pdfpages version %s\n", PACKAGE_VERSION);
        fprintf(stderr, "%s\n", popplerCopyright);
        fprintf(stderr, "%s\n", xpdfCopyright);
        if (!printVersion) {
            printUsage("scholia-pdfpages", "(-insert-blank-after <page> | -delete-page <page>) <PDF-sourcefile> <PDF-destfile>", argDesc);
        }
        if (printVersion || printHelp) {
            return 0;
        }
        return kOtherError;
    }

    const ScholiaPdfPages::Result result = wantsInsert ? ScholiaPdfPages::insertBlankPageAfter(argv[1], argv[2], insertBlankAfter) : ScholiaPdfPages::deletePage(argv[1], argv[2], deletePage);
    if (!result.ok()) {
        fprintf(stderr, "scholia-pdfpages: %s\n", result.message.c_str());
        return kOtherError;
    }
    return 0;
}
