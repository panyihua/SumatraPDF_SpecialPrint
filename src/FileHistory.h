/* Copyright 2013 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#ifndef FileHistory_h
#define FileHistory_h

#include "DisplayState.h"
#include "FileUtil.h"

/* Handling of file history list.

   We keep a mostly infinite list of all (still existing in the file system)
   files that a user has ever opened. For each file we also keep a bunch of
   attributes describing the display state at the time the file was closed.

   We persist this list inside preferences file to something looking like this:

FileStates [
   FilePath =  C:\path\to\file.pdf
   DisplayMode = single page
   PageNo =  1
   ZoomVirtual = 123.4567
   Window State = 2
   ...
]
etc...

    We deserialize this info at startup and serialize when the application
    quits.
*/

// number of most recently used files that will be shown in the menu
// (and remembered in the preferences file, if just filenames are
//  to be remembered and not individual view settings per document)
#define FILE_HISTORY_MAX_RECENT     10

// maximum number of most frequently used files that will be shown on the
// Frequent Read list (space permitting)
#define FILE_HISTORY_MAX_FREQUENT   10

// maximum number of files to remember in total
// (to keep the settings file within reasonable bounds)
#define FILE_HISTORY_MAX_FILES 1000

class FileHistory {
    // owned by gGlobalPrefs->fileStates
    Vec<DisplayState *> *states;

    // sorts the most often used files first
    static int cmpOpenCount(const void *a, const void *b) {
        DisplayState *dsA = *(DisplayState **)a;
        DisplayState *dsB = *(DisplayState **)b;
        // sort pinned documents before unpinned ones
        if (dsA->isPinned != dsB->isPinned)
            return dsA->isPinned ? -1 : 1;
        // sort pinned documents alphabetically
        if (dsA->isPinned)
            return str::CmpNatural(path::GetBaseName(dsA->filePath), path::GetBaseName(dsB->filePath));
        // sort often opened documents first
        if (dsA->openCount != dsB->openCount)
            return dsB->openCount - dsA->openCount;
        // use recency as the criterion in case of equal open counts
        return dsA->index < dsB->index ? -1 : 1;
    }

public:
    FileHistory() : states(NULL) { }
    ~FileHistory() { }

    void Clear(bool keepFavorites) {
        if (!states)
            return;
        for (size_t i = 0; i < states->Count(); i++) {
            if (keepFavorites && states->At(i)->favorites->Count() > 0)
                continue;
            DeleteDisplayState(states->At(i));
        }
        states->Reset();
    }

    void Append(DisplayState *state) { states->Append(state); }
    void Remove(DisplayState *state) { states->Remove(state); }

    DisplayState *Get(size_t index) const {
        if (index < states->Count())
            return states->At(index);
        return NULL;
    }

    DisplayState *Find(const WCHAR *filePath, size_t *idxOut=NULL) const {
        for (size_t i = 0; i < states->Count(); i++) {
            if (str::EqI(states->At(i)->filePath, filePath)) {
                if (idxOut)
                    *idxOut = i;
                return states->At(i);
            }
        }
        return NULL;
    }

    DisplayState *MarkFileLoaded(const WCHAR *filePath) {
        CrashIf(!filePath);
        // if a history entry with the same name already exists,
        // then reuse it. That way we don't have duplicates and
        // the file moves to the front of the list
        DisplayState *state = Find(filePath);
        if (!state) {
            state = NewDisplayState(filePath);
            state->useDefaultState = true;
        }
        else {
            states->Remove(state);
            state->isMissing = false;
        }
        states->InsertAt(0, state);
        state->openCount++;
        return state;
    }

    bool MarkFileInexistent(const WCHAR *filePath, bool hide=false) {
        assert(filePath);
        DisplayState *state = Find(filePath);
        if (!state)
            return false;
        // move the file history entry to the end of the list
        // of recently opened documents (if it exists at all),
        // so that the user could still try opening it again
        // and so that we don't completely forget the settings,
        // should the file reappear later on
        int newIdx = hide ? INT_MAX : FILE_HISTORY_MAX_RECENT - 1;
        int idx = states->Find(state);
        if (idx < newIdx && state != states->Last()) {
            states->Remove(state);
            if (states->Count() <= (size_t)newIdx)
                states->Append(state);
            else
                states->InsertAt(newIdx, state);
        }
        // also delete the thumbnail and move the link towards the
        // back in the Frequently Read list
        delete state->thumbnail;
        state->thumbnail = NULL;
        state->openCount >>= 2;
        state->isMissing = hide;
        return true;
    }

    // returns a shallow copy of the file history list, sorted
    // by open count (which has a pre-multiplied recency factor)
    // and with all missing states filtered out
    // caller needs to delete the result (but not the contained states)
    void GetFrequencyOrder(Vec<DisplayState *>& list) {
        CrashIf(list.Count() > 0);
        size_t i = 0;
        for (DisplayState **ds = states->IterStart(); ds; ds = states->IterNext()) {
            (*ds)->index = i++;
            if (!(*ds)->isMissing || (*ds)->isPinned)
                list.Append(*ds);
        }
        list.Sort(cmpOpenCount);
    }

    // removes file history entries which shouldn't be saved anymore
    // (see the loop below for the details)
    void Purge(bool alwaysUseDefaultState=false) {
        // minOpenCount is set to the number of times a file must have been
        // opened to be kept (provided that there is no other valuable
        // information about the file to be remembered)
        int minOpenCount = 0;
        if (alwaysUseDefaultState) {
            Vec<DisplayState *> frequencyList;
            GetFrequencyOrder(frequencyList);
            if (frequencyList.Count() > FILE_HISTORY_MAX_RECENT)
                minOpenCount = frequencyList.At(FILE_HISTORY_MAX_FREQUENT)->openCount / 2;
        }

        for (size_t j = states->Count(); j > 0; j--) {
            DisplayState *state = states->At(j - 1);
            // never forget pinned documents, documents we've remembered a password for and
            // documents for which there are favorites
            if (state->isPinned || state->decryptionKey != NULL || state->favorites->Count() > 0)
                continue;
            // forget about missing documents without valuable state
            if (state->isMissing && (alwaysUseDefaultState || state->useDefaultState))
                states->RemoveAt(j - 1);
            // forget about files last opened longer ago than the last FILE_HISTORY_MAX_FILES ones
            else if (j > FILE_HISTORY_MAX_FILES)
                states->RemoveAt(j - 1);
            // forget about files that were hardly used (and without valuable state)
            else if (alwaysUseDefaultState && state->openCount < minOpenCount && j > FILE_HISTORY_MAX_RECENT)
                states->RemoveAt(j - 1);
            else
                continue;
            DeleteDisplayState(state);
        }
    }

    void UpdateStatesSource(Vec<DisplayState *> *states) {
        this->states = states;
    }
};

#endif
