#include "app/App.hpp"
#include "utils/Files.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace br {
SDL_Rect App::pageScreenRect() const {
    if (!view_.bitmap)
        return {};
    float scale = std::min(float(width()) / view_.bitmap->width,
                           float(height()) / view_.bitmap->height);
    if (view_.fixed)
        scale *= zoom_;
    int w = int(view_.bitmap->width * scale);
    int h = int(view_.bitmap->height * scale);
    return {width() / 2 - w / 2 + int(panX_), height() / 2 - h / 2 + int(panY_), w, h};
}

void App::drawHighlights() {
    if (activePath_.empty() || !view_.bitmap)
        return;
    auto page = pageScreenRect();
    if (page.w <= 0 || page.h <= 0)
        return;
    auto notes = notes_.forBook(activePath_);
    for (auto& note : notes) {
        if (note.anchor.page != view_.position.page)
            continue;
        if (note.anchor.rects.empty()) {
            ui_->rect({page.x + page.w - 26, page.y + 10, 16, 16}, ui_->colors.accent);
            continue;
        }
        for (auto& rect : note.anchor.rects) {
            SDL_Rect box{page.x + int(rect.x0 * page.w), page.y + int(rect.y0 * page.h),
                         std::max(1, int((rect.x1 - rect.x0) * page.w)),
                         std::max(1, int((rect.y1 - rect.y0) * page.h))};
            bool active = note.id == activeNoteId_;
            ui_->rect(box, {255, 214, 10, uint8_t(active ? 110 : 60)});
            SDL_SetRenderDrawColor(renderer_, ui_->colors.accent.r, ui_->colors.accent.g,
                                   ui_->colors.accent.b, active ? 220 : 120);
            SDL_RenderDrawRect(renderer_, &box);
        }
    }
}

void App::startSelection() {
    if (activePath_.empty() || !view_.pages) {
        message_ = "Open a book first";
        return;
    }
    selecting_ = true;
    selectionTouchActive_ = false;
    selStartX_ = selStartY_ = selEndX_ = selEndY_ = 0;
    overlay_ = false;
    message_.clear();
}

void App::finishSelection(bool pageOnly) {
    if (!selecting_)
        return;
    selecting_ = false;
    selectionTouchActive_ = false;
    NoteAnchor anchor;
    anchor.page = view_.position.page;
    anchor.chapter = view_.position.chapter;
    anchor.offset = view_.position.offset;
    anchor.progress = view_.position.progress;
    anchor.fixed = view_.fixed;
    if (!pageOnly) {
        auto page = pageScreenRect();
        float x0 = std::min(selStartX_, selEndX_), y0 = std::min(selStartY_, selEndY_);
        float x1 = std::max(selStartX_, selEndX_), y1 = std::max(selStartY_, selEndY_);
        if (page.w > 0 && page.h > 0 && x1 - x0 > 8 && y1 - y0 > 8) {
            NoteRect rect;
            rect.x0 = std::clamp((x0 - page.x) / float(page.w), 0.f, 1.f);
            rect.y0 = std::clamp((y0 - page.y) / float(page.h), 0.f, 1.f);
            rect.x1 = std::clamp((x1 - page.x) / float(page.w), 0.f, 1.f);
            rect.y1 = std::clamp((y1 - page.y) / float(page.h), 0.f, 1.f);
            anchor.rects.push_back(rect);
        }
    }
    startNoteEditor(anchor, "", "");
}

void App::startNoteEditor(const NoteAnchor& anchor, const std::string& id,
                          const std::string& text) {
    noteAnchor_ = anchor;
    editingNoteId_ = id;
    noteDraft_ = text;
    returnScreen_ = screen_;
#ifdef __SWITCH__
    SwkbdConfig keyboard;
    char buffer[2048]{};
    if (R_FAILED(swkbdCreate(&keyboard, 0))) {
        message_ = "Cannot open keyboard";
        return;
    }
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, "BookReader note");
    swkbdConfigSetGuideText(&keyboard, "Write your note");
    swkbdConfigSetStringLenMax(&keyboard, 2000);
    swkbdConfigSetInitialText(&keyboard, noteDraft_.c_str());
    Result result = swkbdShow(&keyboard, buffer, sizeof(buffer));
    swkbdClose(&keyboard);
    if (R_SUCCEEDED(result)) {
        noteDraft_ = buffer;
        commitNote();
    }
#else
    setScreen(Screen::NoteEditor);
    SDL_StartTextInput();
#endif
}

void App::commitNote() {
    if (noteDraft_.empty() && noteAnchor_.rects.empty()) {
        cancelNote();
        return;
    }
    std::string id = editingNoteId_;
    if (id.empty())
        id = notes_.add(activePath_, view_.title, noteDraft_, noteAnchor_);
    else
        notes_.update(id, noteDraft_, noteAnchor_);
    if (id.empty() || !notes_.save()) {
        message_ = "Cannot save notes";
        return;
    }
    activeNoteId_ = id;
    noteDraft_.clear();
    editingNoteId_.clear();
    SDL_StopTextInput();
    setScreen(returnScreen_);
    overlay_ = false;
    message_ = "Note saved";
}

void App::cancelNote() {
    noteDraft_.clear();
    editingNoteId_.clear();
    noteAnchor_ = {};
    SDL_StopTextInput();
    setScreen(returnScreen_);
    overlay_ = false;
}

Position App::anchorPosition(const NoteAnchor& anchor) const {
    Position position;
    position.page = anchor.page;
    position.chapter = anchor.chapter;
    position.offset = anchor.offset;
    position.progress = anchor.progress;
    position.zoom = 1;
    return position;
}

void App::openNote(const Note& note) {
    auto position = anchorPosition(note.anchor);
    if (activePath_ != note.bookPath) {
        auto& book = config_.books[note.bookPath];
        book.path = note.bookPath;
        book.position = position;
        book.hasSettings = false;
        openBook(note.bookPath);
        activeNoteId_ = note.id;
        setScreen(Screen::Reader);
        overlay_ = false;
    } else {
        Request request;
        request.kind = Request::Locate;
        request.position = position;
        worker_->submit(request);
        activeNoteId_ = note.id;
        setScreen(Screen::Reader);
        overlay_ = false;
    }
}

void App::editFocusedNote() {
    auto notes = notes_.all();
    if (notesFocus_ < 0 || notesFocus_ >= int(notes.size()))
        return;
    auto note = notes[notesFocus_];
    startNoteEditor(note.anchor, note.id, note.text);
}

void App::deleteFocusedNote() {
    auto notes = notes_.all();
    if (notesFocus_ < 0 || notesFocus_ >= int(notes.size()))
        return;
    auto note = notes[notesFocus_];
    if (!notes_.remove(note.id))
        return;
    if (!notes_.save()) {
        message_ = "Cannot save notes";
        return;
    }
    if (activeNoteId_ == note.id)
        activeNoteId_.clear();
    notesFocus_ = -1;
    message_ = "Note deleted";
}

void App::drawNotes() {
    auto& u = *ui_;
    u.text(tr("Notes"), 40, 24, 36, u.colors.text, width() - 280);
    u.button({width() - 220, 25, 180, 58}, tr("Back"), [this] { setScreen(Screen::Home); });
    auto notes = notes_.all();
    const int top = 120, rowHeight = 96;
    int rows = std::max(1, (height() - top - 110) / rowHeight);
    notesStart_ = std::clamp(notesStart_, 0, std::max(0, int(notes.size()) - rows));
    notesFocus_ = -1;
    if (notes.empty())
        u.text(tr("No notes"), 44, 140, 26, u.colors.muted, width() - 88);
    for (int row = 0; row < rows && notesStart_ + row < int(notes.size()); ++row) {
        int index = notesStart_ + row;
        auto note = notes[index];
        int y = top + row * rowHeight;
        u.button({40, y, width() - 80, 84}, "", [this, note] { openNote(note); });
        u.text(note.bookTitle.empty() ? basename(note.bookPath) : note.bookTitle, 60, y + 8, 22,
               u.colors.text, width() - 340);
        std::string meta = tr("Page") + " " + std::to_string(note.anchor.page + 1);
        if (!note.anchor.rects.empty())
            meta += "  ·  " + tr("Highlight");
        u.text(meta, width() - 280, y + 8, 18, u.colors.muted, 220);
        auto excerpt = NoteStore::excerpt(note.text);
        if (excerpt.empty())
            excerpt = tr("Highlight");
        u.text(excerpt, 60, y + 40, 18, u.colors.muted, width() - 120);
        if (focus_ == row + 1)
            notesFocus_ = index;
    }
    u.button({40, height() - 86, 160, 58}, tr("Previous"),
             [this, rows] { notesStart_ = std::max(0, notesStart_ - rows); });
    u.button({218, height() - 86, 160, 58}, tr("Next"), [this, rows] {
        notesStart_ = std::min(std::max(0, int(notes_.notes().size()) - rows), notesStart_ + rows);
    });
    u.text(tr("X: edit · Y: delete"), width() - 420, height() - 72, 20, u.colors.muted, 380);
}

void App::drawNoteEditor() {
    auto& u = *ui_;
    u.text(tr("Note"), 40, 24, 36, u.colors.text, width() - 280);
    u.button({width() - 220, 25, 180, 58}, tr("Back"), [this] { cancelNote(); });
    u.text(tr("Page") + " " + std::to_string(noteAnchor_.page + 1), 40, 80, 22, u.colors.muted,
           width() - 80);
    u.rect({40, 120, width() - 80, height() - 270}, u.colors.panel);
    u.text(noteDraft_.empty() ? tr("Write your note") : noteDraft_, 60, 140, 26, u.colors.text,
           width() - 120);
    u.button({40, height() - 130, 200, 60}, tr("Save"), [this] { commitNote(); });
    u.button({260, height() - 130, 200, 60}, tr("Cancel"), [this] { cancelNote(); });
}
} // namespace br
