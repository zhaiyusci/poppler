//========================================================================
//
// ScholiaPdfPages.h
//
// This file is licensed under the GPLv2 or later
//
//========================================================================

#ifndef SCHOLIA_PDFPAGES_H
#define SCHOLIA_PDFPAGES_H

#include <string>
#include <vector>

namespace ScholiaPdfPages
{

enum class Error
{
    None,
    InvalidArguments,
    DamagedInput,
    EncryptedInput,
    PageError,
    IoError,
    WriteError,
};

struct Result
{
    Error error = Error::None;
    std::string message;
    int inputPageCount = 0;
    int outputPageCount = 0;

    bool ok() const { return error == Error::None; }
};

Result insertBlankPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber);
Result insertBlankPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, double width, double height);
Result insertPdfPageAfter(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, const std::string &insertedFileName, int pageToInsert);
Result deletePage(const std::string &inputFileName, const std::string &outputFileName, int pageNumber);
Result movePage(const std::string &inputFileName, const std::string &outputFileName, int sourcePageNumber, int destinationPageNumber);
Result reorderPages(const std::string &inputFileName, const std::string &outputFileName, const std::vector<int> &pageOrder);

}

#endif
