#include "notes/Annotations.hpp"
#include "utils/Files.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace br {
namespace {
int64_t integer(const std::string& value, int64_t fallback = 0) {
    try {
        size_t end = 0;
        auto number = std::stoll(value, &end);
        return end == value.size() ? number : fallback;
    } catch (...) {
        return fallback;
    }
}

float number(const std::string& value, float fallback = 0) {
    try {
        size_t end = 0;
        auto result = std::stof(value, &end);
        return end == value.size() && std::isfinite(result) ? result : fallback;
    } catch (...) {
        return fallback;
    }
}

int64_t now() {
    return int64_t(std::time(nullptr));
}
} // namespace

bool NoteStore::parse(const std::string& path) {
    std::ifstream file(path);
    if (!file)
        return false;
    file.seekg(0, std::ios::end);
    if (file.tellg() > 8 * 1024 * 1024)
        return false;
    file.seekg(0);

    Note current;
    std::string line, section;
    bool header = false, complete = false;
    auto finish = [&] {
        if (!current.id.empty()) {
            notes_.push_back(current);
            current = {};
        }
    };

    while (std::getline(file, line)) {
        if (line.size() > 65536)
            return false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "; BookReader notes 1")
            header = true;
        if (line == "; complete")
            complete = true;
        if (line.size() > 1 && line.front() == '[' && line.back() == ']') {
            finish();
            section = line.substr(1, line.size() - 2);
            if (section.rfind("note:", 0) == 0)
                current.id = section.substr(5);
            continue;
        }
        if (section.rfind("note:", 0) != 0)
            continue;
        auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        auto key = line.substr(0, equals);
        auto value = unescape(line.substr(equals + 1));
        if (key == "book")
            current.bookPath = value;
        else if (key == "title")
            current.bookTitle = value;
        else if (key == "text")
            current.text = value;
        else if (key == "page")
            current.anchor.page = int(std::clamp<int64_t>(integer(value), 0, 100000000));
        else if (key == "chapter")
            current.anchor.chapter = int(std::clamp<int64_t>(integer(value), 0, 100000000));
        else if (key == "offset")
            current.anchor.offset = uint64_t(std::max<int64_t>(0, integer(value)));
        else if (key == "progress")
            current.anchor.progress = std::clamp(number(value), 0.f, 1.f);
        else if (key == "fixed")
            current.anchor.fixed = integer(value) != 0;
        else if (key == "created")
            current.created = std::max<int64_t>(0, integer(value));
        else if (key == "modified")
            current.modified = std::max<int64_t>(0, integer(value));
        else if (key == "rects") {
            current.anchor.rects.clear();
            std::stringstream rects(value);
            std::string part;
            while (std::getline(rects, part, ';') && current.anchor.rects.size() < 128) {
                NoteRect rect;
                if (std::sscanf(part.c_str(), "%f,%f,%f,%f", &rect.x0, &rect.y0, &rect.x1, &rect.y1) ==
                    4) {
                    rect.x0 = std::clamp(rect.x0, 0.f, 1.f);
                    rect.y0 = std::clamp(rect.y0, 0.f, 1.f);
                    rect.x1 = std::clamp(rect.x1, 0.f, 1.f);
                    rect.y1 = std::clamp(rect.y1, 0.f, 1.f);
                    current.anchor.rects.push_back(rect);
                }
            }
        }
    }
    finish();
    if (!header || !complete) {
        notes_.clear();
        return false;
    }
    for (auto& note : notes_) {
        if (!note.created)
            note.created = note.modified;
        if (!note.modified)
            note.modified = note.created;
    }
    nextId_ = 1;
    for (auto& note : notes_) {
        auto id = uint64_t(std::max<int64_t>(0, integer(note.id)));
        if (id >= nextId_)
            nextId_ = id + 1;
    }
    return true;
}

bool NoteStore::load() {
    notes_.clear();
    nextId_ = 1;
    if (parse(path_))
        return true;
    notes_.clear();
    nextId_ = 1;
    return parse(path_ + ".bak");
}

bool NoteStore::save() {
    std::ostringstream out;
    out << "; BookReader notes 1\n";
    for (auto& note : notes_) {
        out << "\n[note:" << note.id << "]\n"
            << "book=" << escape(note.bookPath) << "\n"
            << "title=" << escape(note.bookTitle) << "\n"
            << "text=" << escape(note.text) << "\n"
            << "page=" << note.anchor.page << "\n"
            << "chapter=" << note.anchor.chapter << "\n"
            << "offset=" << note.anchor.offset << "\n"
            << "progress=" << note.anchor.progress << "\n"
            << "fixed=" << (note.anchor.fixed ? 1 : 0) << "\n"
            << "rects=";
        for (size_t i = 0; i < note.anchor.rects.size(); ++i) {
            auto& rect = note.anchor.rects[i];
            if (i)
                out << ';';
            out << std::fixed << std::setprecision(4) << rect.x0 << ',' << rect.y0 << ','
                << rect.x1 << ',' << rect.y1;
        }
        out << "\ncreated=" << note.created << "\nmodified=" << note.modified << "\n";
    }
    out << "; complete\n";

    std::ifstream previous(path_, std::ios::binary);
    if (previous) {
        std::ostringstream backup;
        backup << previous.rdbuf();
        if (!backup.str().empty())
            atomicWrite(path_ + ".bak", backup.str());
    }
    return atomicWrite(path_, out.str());
}

std::vector<Note> NoteStore::all() const {
    auto result = notes_;
    std::sort(result.begin(), result.end(), [](const Note& a, const Note& b) {
        if (a.modified != b.modified)
            return a.modified > b.modified;
        return a.id > b.id;
    });
    return result;
}

std::vector<Note> NoteStore::forBook(const std::string& bookPath) const {
    std::vector<Note> result;
    for (auto& note : notes_)
        if (note.bookPath == bookPath)
            result.push_back(note);
    return result;
}

const Note* NoteStore::find(const std::string& id) const {
    for (auto& note : notes_)
        if (note.id == id)
            return &note;
    return nullptr;
}

std::string NoteStore::add(const std::string& bookPath, const std::string& bookTitle,
                           const std::string& text, const NoteAnchor& anchor) {
    if (notes_.size() >= 5000) {
        auto oldest = std::min_element(notes_.begin(), notes_.end(),
                                       [](const Note& a, const Note& b) {
                                           return a.modified < b.modified;
                                       });
        if (oldest != notes_.end())
            notes_.erase(oldest);
    }
    Note note;
    note.id = std::to_string(nextId_++);
    note.bookPath = bookPath;
    note.bookTitle = bookTitle;
    note.text = text;
    note.anchor = anchor;
    note.created = note.modified = now();
    notes_.push_back(std::move(note));
    return notes_.back().id;
}

bool NoteStore::update(const std::string& id, const std::string& text,
                       const NoteAnchor& anchor) {
    for (auto& note : notes_) {
        if (note.id == id) {
            note.text = text;
            note.anchor = anchor;
            note.modified = now();
            return true;
        }
    }
    return false;
}

bool NoteStore::remove(const std::string& id) {
    auto it = std::find_if(notes_.begin(), notes_.end(),
                           [&](const Note& note) { return note.id == id; });
    if (it == notes_.end())
        return false;
    notes_.erase(it);
    return true;
}

std::string NoteStore::excerpt(const std::string& text, size_t max) {
    std::string out;
    bool space = false;
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ') {
            if (space)
                continue;
            space = true;
        } else
            space = false;
        out += c;
        if (out.size() >= max)
            break;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    if (out.size() >= max) {
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
        out += "...";
    }
    return out;
}
} // namespace br
