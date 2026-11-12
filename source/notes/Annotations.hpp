#pragma once
#include "utils/Types.hpp"
#include <string>
#include <vector>

namespace br {
struct Note {
    std::string id, bookPath, bookTitle, text;
    NoteAnchor anchor;
    int64_t created = 0, modified = 0;
};

// Persistent annotation store. One INI file in config/notes.ini; no external
// dependencies, atomic writes and escaped UTF-8 fields.
class NoteStore {
    std::string path_;
    std::vector<Note> notes_;
    uint64_t nextId_ = 1;
    bool parse(const std::string& path);

  public:
    explicit NoteStore(std::string path) : path_(std::move(path)) {}
    bool load();
    bool save();
    const std::vector<Note>& notes() const { return notes_; }
    std::vector<Note> all() const;
    std::vector<Note> forBook(const std::string& bookPath) const;
    const Note* find(const std::string& id) const;
    std::string add(const std::string& bookPath, const std::string& bookTitle,
                    const std::string& text, const NoteAnchor& anchor);
    bool update(const std::string& id, const std::string& text, const NoteAnchor& anchor);
    bool remove(const std::string& id);
    static std::string excerpt(const std::string& text, size_t max = 80);
};
} // namespace br
