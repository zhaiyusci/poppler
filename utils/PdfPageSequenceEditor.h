//========================================================================
//
// PdfPageSequenceEditor.h
//
// This file is licensed under the GPLv2 or later
//
//========================================================================

#ifndef PDF_PAGE_SEQUENCE_EDITOR_H
#define PDF_PAGE_SEQUENCE_EDITOR_H

#include <string>
#include <vector>

namespace PdfPageSequenceEditor
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

enum class NamedDestinationConflictPolicy
{
    KeepNames,
    AddSuffixes,
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
Result insertPdfPageAfter(const std::string &inputFileName,
                          const std::string &outputFileName,
                          int pageNumber,
                          const std::string &insertedFileName,
                          int pageToInsert,
                          NamedDestinationConflictPolicy conflictPolicy = NamedDestinationConflictPolicy::KeepNames);
Result combinePdfFiles(const std::vector<std::string> &inputFileNames,
                       const std::string &outputFileName,
                       NamedDestinationConflictPolicy conflictPolicy = NamedDestinationConflictPolicy::KeepNames);
int pageCount(const std::string &inputFileName, std::string *errorMessage = nullptr);
Result deletePage(const std::string &inputFileName, const std::string &outputFileName, int pageNumber);
Result movePage(const std::string &inputFileName, const std::string &outputFileName, int sourcePageNumber, int destinationPageNumber);
Result reorderPages(const std::string &inputFileName, const std::string &outputFileName, const std::vector<int> &pageOrder);
Result rotatePage(const std::string &inputFileName, const std::string &outputFileName, int pageNumber, int rotationDegrees);

}

#endif
