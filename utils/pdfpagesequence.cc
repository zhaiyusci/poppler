//========================================================================
//
// pdfpagesequence.cc
//
// This file is licensed under the GPLv2 or later
//
//========================================================================

#include "config.h"
#include <poppler-config.h>

#include <cstdio>

#include "PdfPageSequenceEditor.h"
#include "parseargs.h"

static int insertBlankAfter = -1;
static double insertBlankWidth = 0;
static double insertBlankHeight = 0;
static int insertPdfAfter = -1;
static char insertPdfFile[4096] = "";
static int insertPdfPage = -1;
static int deletePage = -1;
static int movePageFrom = -1;
static int movePageTo = -1;
static bool printVersion = false;
static bool printHelp = false;

static const ArgDesc argDesc[] = { { "-insert-blank-after", argInt, &insertBlankAfter, 0, "insert one blank page after the given 1-based page number; use 0 to insert before page 1" },
                                   { "-blank-width", argFP, &insertBlankWidth, 0, "width in PDF points for the inserted blank page" },
                                   { "-blank-height", argFP, &insertBlankHeight, 0, "height in PDF points for the inserted blank page" },
                                   { "-insert-pdf-page-after", argInt, &insertPdfAfter, 0, "insert one page from another PDF after the given 1-based page number; use 0 to insert before page 1" },
                                   { "-insert-pdf-file", argString, insertPdfFile, sizeof(insertPdfFile), "PDF file to import a page from" },
                                   { "-insert-pdf-page", argInt, &insertPdfPage, 0, "1-based page number to import from -insert-pdf-file" },
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
    const bool wantsInsertPdf = insertPdfAfter >= 0 || insertPdfFile[0] != '\0' || insertPdfPage >= 0;
    const bool wantsDelete = deletePage >= 0;
    const bool wantsMove = movePageFrom >= 0 || movePageTo >= 0;
    if (!parseOK || argc != 3 || printVersion || printHelp || static_cast<int>(wantsInsert) + static_cast<int>(wantsInsertPdf) + static_cast<int>(wantsDelete) + static_cast<int>(wantsMove) != 1
        || (wantsMove && (movePageFrom < 0 || movePageTo < 0)) || (wantsInsertPdf && (insertPdfAfter < 0 || insertPdfFile[0] == '\0' || insertPdfPage < 1))) {
        fprintf(stderr, "pdfpagesequence version %s\n", PACKAGE_VERSION);
        fprintf(stderr, "%s\n", popplerCopyright);
        fprintf(stderr, "%s\n", xpdfCopyright);
        if (!printVersion) {
            printUsage("pdfpagesequence", "(-insert-blank-after <page> [-blank-width <points> -blank-height <points>] | -insert-pdf-page-after <page> -insert-pdf-file <PDF> -insert-pdf-page <page> | -delete-page <page> | -move-page-from <page> -move-page-to <page>) <PDF-sourcefile> <PDF-destfile>", argDesc);
        }
        if (printVersion || printHelp) {
            return 0;
        }
        return kOtherError;
    }

    PdfPageSequenceEditor::Result result;
    if (wantsInsert) {
        result = insertBlankWidth > 0 || insertBlankHeight > 0 ? PdfPageSequenceEditor::insertBlankPageAfter(argv[1], argv[2], insertBlankAfter, insertBlankWidth, insertBlankHeight) : PdfPageSequenceEditor::insertBlankPageAfter(argv[1], argv[2], insertBlankAfter);
    } else if (wantsInsertPdf) {
        result = PdfPageSequenceEditor::insertPdfPageAfter(argv[1], argv[2], insertPdfAfter, insertPdfFile, insertPdfPage);
    } else if (wantsDelete) {
        result = PdfPageSequenceEditor::deletePage(argv[1], argv[2], deletePage);
    } else {
        result = PdfPageSequenceEditor::movePage(argv[1], argv[2], movePageFrom, movePageTo);
    }
    if (!result.ok()) {
        fprintf(stderr, "pdfpagesequence: %s\n", result.message.c_str());
        return kOtherError;
    }
    return 0;
}
