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
static int movePageFrom = -1;
static int movePageTo = -1;
static bool printVersion = false;
static bool printHelp = false;

static const ArgDesc argDesc[] = { { "-insert-blank-after", argInt, &insertBlankAfter, 0, "insert one blank page after the given 1-based page number; use 0 to insert before page 1" },
                                   { "-delete-page", argInt, &deletePage, 0, "delete the given 1-based page number" },
                                   { "-move-page-from", argInt, &movePageFrom, 0, "move the given 1-based page number" },
                                   { "-move-page-to", argInt, &movePageTo, 0, "final 1-based destination position for -move-page-from" },
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
    const bool wantsMove = movePageFrom >= 0 || movePageTo >= 0;
    if (!parseOK || argc != 3 || printVersion || printHelp || static_cast<int>(wantsInsert) + static_cast<int>(wantsDelete) + static_cast<int>(wantsMove) != 1 || (wantsMove && (movePageFrom < 0 || movePageTo < 0))) {
        fprintf(stderr, "scholia-pdfpages version %s\n", PACKAGE_VERSION);
        fprintf(stderr, "%s\n", popplerCopyright);
        fprintf(stderr, "%s\n", xpdfCopyright);
        if (!printVersion) {
            printUsage("scholia-pdfpages", "(-insert-blank-after <page> | -delete-page <page> | -move-page-from <page> -move-page-to <page>) <PDF-sourcefile> <PDF-destfile>", argDesc);
        }
        if (printVersion || printHelp) {
            return 0;
        }
        return kOtherError;
    }

    const ScholiaPdfPages::Result result = wantsInsert ? ScholiaPdfPages::insertBlankPageAfter(argv[1], argv[2], insertBlankAfter) : (wantsDelete ? ScholiaPdfPages::deletePage(argv[1], argv[2], deletePage) : ScholiaPdfPages::movePage(argv[1], argv[2], movePageFrom, movePageTo));
    if (!result.ok()) {
        fprintf(stderr, "scholia-pdfpages: %s\n", result.message.c_str());
        return kOtherError;
    }
    return 0;
}
