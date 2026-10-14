#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "TextBuffer.h"

#include <string>

/// Document: wraps the text buffer into something that can be read from and
/// written back to a real file.
/// we always read as UTF-8 and there is no encoding picker, so a BOM at the head
/// of the file gets dropped for you. inside the buffer a line break is always a
/// single '\n' -- we only grow it into CRLF at the very last moment, and only
/// when we happen to be on Windows.
class Document
{
public:
    /// Creates an empty, unnamed document -- save() refuses to run until load()
    /// hands it a real path.
    Document();

    /// Reads a file and replaces the current content with it.
    /// whatever was in the buffer is destroyed first, so don't call this while
    /// the user still has unsaved edits. false means the file could not be
    /// opened at all -- look at error() for the reason.
    bool load(const std::string &path);

    /// Saves back to the current path, returns false when there is no name yet.
    /// there is no Save As any more, an unnamed document can't be written.
    /// we write straight to the current path, so the old file is gone the moment
    /// fopen succeeds -- no temp file, no rename dance to fall back on.
    bool save();

    /// Clears the content and cuts the file association.
    /// the path is dropped on purpose, so a later save() fails loudly instead of
    /// quietly overwriting the file you were just editing.
    void createNew();

    /// Points the document at another file WITHOUT touching the content.
    /// this is what Save As needs: save() only ever writes to the current path,
    /// so changing that path is the only way to write somewhere else. the path is
    /// not checked here -- a bad one is reported by the next save().
    void setPath(const std::string &path) { _path = path; }

    /// The text buffer, we hand out a raw pointer; it lives inside the document
    /// and never moves, so it stays valid for as long as the Document does.
    TextBuffer *buffer() { return &_buffer; }

    /// Current file path, an empty string while the document is unnamed.
    const std::string &path() const { return _path; }

    /// Error message from the last read/write, for the console to print.
    /// it gets wiped by the next successful load() or save(), so print it right away.
    const std::string &error() const { return _error; }

    /// Whether a file is associated, false just means the path is empty --
    /// it says nothing about the content being empty.
    bool named() const { return !_path.empty(); }

private:
    TextBuffer _buffer;  /// the text buffer
    std::string _path;   /// current file path
    std::string _error;  /// most recent error
};

#endif // DOCUMENT_H
