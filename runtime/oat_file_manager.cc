/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "oat_file_manager.h"

#include <memory>
#include <queue>
#include <vector>

#include "base/logging.h"
#include "base/stl_util.h"
#include "dex_file.h"
#include "gc/space/image_space.h"
#include "oat_file_assistant.h"
#include "thread-inl.h"

namespace art {

// For b/21333911.
static constexpr bool kDuplicateClassesCheck = false;

const OatFile* OatFileManager::RegisterOatFile(std::unique_ptr<const OatFile> oat_file) {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  DCHECK(oat_file != nullptr);
  if (kIsDebugBuild) {
    for (const std::unique_ptr<const OatFile>& existing : oat_files_) {
      CHECK_NE(oat_file.get(), existing.get()) << oat_file->GetLocation();
      // Check that we don't have an oat file with the same address. Copies of the same oat file
      // should be loaded at different addresses.
      CHECK_NE(oat_file->Begin(), existing->Begin()) << "Oat file already mapped at that location";
    }
  }
  have_non_pic_oat_file_ = have_non_pic_oat_file_ || !oat_file->IsPic();
  oat_files_.push_back(std::move(oat_file));
  return oat_files_.back().get();
}

const OatFile* OatFileManager::FindOpenedOatFileFromOatLocation(const std::string& oat_location)
    const {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
    if (oat_file->GetLocation() == oat_location) {
      return oat_file.get();
    }
  }
  return nullptr;
}

const OatFile* OatFileManager::GetBootOatFile() const {
  gc::space::ImageSpace* image_space = Runtime::Current()->GetHeap()->GetImageSpace();
  if (image_space == nullptr) {
    return nullptr;
  }
  return image_space->GetOatFile();
}

const OatFile* OatFileManager::GetPrimaryOatFile() const {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  const OatFile* boot_oat_file = GetBootOatFile();
  if (boot_oat_file != nullptr) {
    for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
      if (oat_file.get() != boot_oat_file) {
        return oat_file.get();
      }
    }
  }
  return nullptr;
}

OatFileManager::~OatFileManager() {
}

const OatFile* OatFileManager::RegisterImageOatFile(gc::space::ImageSpace* space) {
  return RegisterOatFile(space->ReleaseOatFile());
}

class DexFileAndClassPair : ValueObject {
 public:
  DexFileAndClassPair(const DexFile* dex_file, size_t current_class_index, bool from_loaded_oat)
     : cached_descriptor_(GetClassDescriptor(dex_file, current_class_index)),
       dex_file_(dex_file),
       current_class_index_(current_class_index),
       from_loaded_oat_(from_loaded_oat) {}

  DexFileAndClassPair(DexFileAndClassPair&& rhs) {
    *this = std::move(rhs);
  }

  DexFileAndClassPair& operator=(DexFileAndClassPair&& rhs) {
    cached_descriptor_ = rhs.cached_descriptor_;
    dex_file_ = std::move(rhs.dex_file_);
    current_class_index_ = rhs.current_class_index_;
    from_loaded_oat_ = rhs.from_loaded_oat_;
    return *this;
  }

  const char* GetCachedDescriptor() const {
    return cached_descriptor_;
  }

  bool operator<(const DexFileAndClassPair& rhs) const {
    const int cmp = strcmp(cached_descriptor_, rhs.cached_descriptor_);
    if (cmp != 0) {
      // Note that the order must be reversed. We want to iterate over the classes in dex files.
      // They are sorted lexicographically. Thus, the priority-queue must be a min-queue.
      return cmp > 0;
    }
    return dex_file_ < rhs.dex_file_;
  }

  bool DexFileHasMoreClasses() const {
    return current_class_index_ + 1 < dex_file_->NumClassDefs();
  }

  void Next() {
    ++current_class_index_;
  }

  size_t GetCurrentClassIndex() const {
    return current_class_index_;
  }

  bool FromLoadedOat() const {
    return from_loaded_oat_;
  }

  const DexFile* GetDexFile() const {
    return dex_file_.get();
  }

 private:
  static const char* GetClassDescriptor(const DexFile* dex_file, size_t index) {
    DCHECK(IsUint<16>(index));
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(static_cast<uint16_t>(index));
    return dex_file->StringByTypeIdx(class_def.class_idx_);
  }

  const char* cached_descriptor_;
  std::unique_ptr<const DexFile> dex_file_;
  size_t current_class_index_;
  bool from_loaded_oat_;  // We only need to compare mismatches between what we load now
                          // and what was loaded before. Any old duplicates must have been
                          // OK, and any new "internal" duplicates are as well (they must
                          // be from multidex, which resolves correctly).
};

static void AddDexFilesFromOat(const OatFile* oat_file,
                               bool already_loaded,
                               /*out*/std::priority_queue<DexFileAndClassPair>* heap) {
  for (const OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
    std::string error;
    std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error);
    if (dex_file == nullptr) {
      LOG(WARNING) << "Could not create dex file from oat file: " << error;
    } else if (dex_file->NumClassDefs() > 0U) {
      heap->emplace(dex_file.release(), /*current_class_index*/0U, already_loaded);
    }
  }
}

static void AddNext(/*inout*/DexFileAndClassPair* original,
                    /*inout*/std::priority_queue<DexFileAndClassPair>* heap) {
  if (original->DexFileHasMoreClasses()) {
    original->Next();
    heap->push(std::move(*original));
  }
}

// Check for class-def collisions in dex files.
//
// This works by maintaining a heap with one class from each dex file, sorted by the class
// descriptor. Then a dex-file/class pair is continually removed from the heap and compared
// against the following top element. If the descriptor is the same, it is now checked whether
// the two elements agree on whether their dex file was from an already-loaded oat-file or the
// new oat file. Any disagreement indicates a collision.
bool OatFileManager::HasCollisions(const OatFile* oat_file,
                                   std::string* error_msg /*out*/) const {
  DCHECK(oat_file != nullptr);
  DCHECK(error_msg != nullptr);
  if (!kDuplicateClassesCheck) {
    return false;
  }

  // Dex files are registered late - once a class is actually being loaded. We have to compare
  // against the open oat files. Take the oat_file_manager_lock_ that protects oat_files_ accesses.
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);

  std::priority_queue<DexFileAndClassPair> queue;

  // Add dex files from already loaded oat files, but skip boot.
  const OatFile* boot_oat = GetBootOatFile();
  for (const std::unique_ptr<const OatFile>& loaded_oat_file : oat_files_) {
    if (loaded_oat_file.get() != boot_oat) {
      AddDexFilesFromOat(loaded_oat_file.get(), /*already_loaded*/true, &queue);
    }
  }

  if (queue.empty()) {
    // No other oat files, return early.
    return false;
  }

  // Add dex files from the oat file to check.
  AddDexFilesFromOat(oat_file, /*already_loaded*/false, &queue);

  // Now drain the queue.
  while (!queue.empty()) {
    // Modifying the top element is only safe if we pop right after.
    DexFileAndClassPair compare_pop(std::move(const_cast<DexFileAndClassPair&>(queue.top())));
    queue.pop();

    // Compare against the following elements.
    while (!queue.empty()) {
      DexFileAndClassPair top(std::move(const_cast<DexFileAndClassPair&>(queue.top())));

      if (strcmp(compare_pop.GetCachedDescriptor(), top.GetCachedDescriptor()) == 0) {
        // Same descriptor. Check whether it's crossing old-oat-files to new-oat-files.
        if (compare_pop.FromLoadedOat() != top.FromLoadedOat()) {
          *error_msg =
              StringPrintf("Found duplicated class when checking oat files: '%s' in %s and %s",
                           compare_pop.GetCachedDescriptor(),
                           compare_pop.GetDexFile()->GetLocation().c_str(),
                           top.GetDexFile()->GetLocation().c_str());
          return true;
        }
        // Pop it.
        queue.pop();
        AddNext(&top, &queue);
      } else {
        // Something else. Done here.
        break;
      }
    }
    AddNext(&compare_pop, &queue);
  }

  return false;
}

std::vector<std::unique_ptr<const DexFile>> OatFileManager::OpenDexFilesFromOat(
    const char* dex_location,
    const char* oat_location,
    std::vector<std::string>* error_msgs) {
  CHECK(dex_location != nullptr);
  CHECK(error_msgs != nullptr);

  // Verify we aren't holding the mutator lock, which could starve GC if we
  // have to generate or relocate an oat file.
  Locks::mutator_lock_->AssertNotHeld(Thread::Current());

  OatFileAssistant oat_file_assistant(dex_location,
                                      oat_location,
                                      kRuntimeISA,
                                      !Runtime::Current()->IsAotCompiler());

  // Lock the target oat location to avoid races generating and loading the
  // oat file.
  std::string error_msg;
  if (!oat_file_assistant.Lock(/*out*/&error_msg)) {
    // Don't worry too much if this fails. If it does fail, it's unlikely we
    // can generate an oat file anyway.
    VLOG(class_linker) << "OatFileAssistant::Lock: " << error_msg;
  }

  const OatFile* source_oat_file = nullptr;

  // Update the oat file on disk if we can. This may fail, but that's okay.
  // Best effort is all that matters here.
  if (!oat_file_assistant.MakeUpToDate(/*out*/&error_msg)) {
    LOG(WARNING) << error_msg;
  }

  // Get the oat file on disk.
  std::unique_ptr<const OatFile> oat_file(oat_file_assistant.GetBestOatFile().release());
  if (oat_file != nullptr) {
    // Take the file only if it has no collisions, or we must take it because of preopting.
    bool accept_oat_file = !HasCollisions(oat_file.get(), /*out*/ &error_msg);
    if (!accept_oat_file) {
      // Failed the collision check. Print warning.
      if (Runtime::Current()->IsDexFileFallbackEnabled()) {
        LOG(WARNING) << "Found duplicate classes, falling back to interpreter mode for "
                     << dex_location;
      } else {
        LOG(WARNING) << "Found duplicate classes, dex-file-fallback disabled, will be failing to "
                        " load classes for " << dex_location;
      }
      LOG(WARNING) << error_msg;

      // However, if the app was part of /system and preopted, there is no original dex file
      // available. In that case grudgingly accept the oat file.
      if (!DexFile::MaybeDex(dex_location)) {
        accept_oat_file = true;
        LOG(WARNING) << "Dex location " << dex_location << " does not seem to include dex file. "
                     << "Allow oat file use. This is potentially dangerous.";
      }
    }

    if (accept_oat_file) {
      VLOG(class_linker) << "Registering " << oat_file->GetLocation();
      source_oat_file = RegisterOatFile(std::move(oat_file));
    }
  }

  std::vector<std::unique_ptr<const DexFile>> dex_files;

  // Load the dex files from the oat file.
  if (source_oat_file != nullptr) {
    dex_files = oat_file_assistant.LoadDexFiles(*source_oat_file, dex_location);
    if (dex_files.empty()) {
      error_msgs->push_back("Failed to open dex files from " + source_oat_file->GetLocation());
    }
  }

  // Fall back to running out of the original dex file if we couldn't load any
  // dex_files from the oat file.
  if (dex_files.empty()) {
    if (oat_file_assistant.HasOriginalDexFiles()) {
      if (Runtime::Current()->IsDexFileFallbackEnabled()) {
        if (!DexFile::Open(dex_location, dex_location, /*out*/ &error_msg, &dex_files)) {
          LOG(WARNING) << error_msg;
          error_msgs->push_back("Failed to open dex files from " + std::string(dex_location));
        }
      } else {
        error_msgs->push_back("Fallback mode disabled, skipping dex files.");
      }
    } else {
      error_msgs->push_back("No original dex files found for dex location "
          + std::string(dex_location));
    }
  }
  return dex_files;
}

}  // namespace art