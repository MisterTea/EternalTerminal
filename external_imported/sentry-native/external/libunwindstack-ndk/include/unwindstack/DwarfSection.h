/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef _LIBUNWINDSTACK_DWARF_SECTION_H
#define _LIBUNWINDSTACK_DWARF_SECTION_H

#include <stdint.h>

#include <iterator>
#include <map>
#include <unordered_map>

#include <unwindstack/DwarfError.h>
#include <unwindstack/DwarfLocation.h>
#include <unwindstack/DwarfMemory.h>
#include <unwindstack/DwarfStructs.h>

namespace unwindstack {

// Forward declarations.
enum ArchEnum : uint8_t;
class Memory;
class Regs;
template <typename AddressType>
struct RegsInfo;

class DwarfSection {
 public:
  DwarfSection(Memory* memory);
  virtual ~DwarfSection() = default;

  class iterator : public std::iterator<std::bidirectional_iterator_tag, DwarfFde*> {
   public:
    iterator(DwarfSection* section, size_t index) : index_(index) {
      section->GetFdes(&fdes_);
      if (index_ == static_cast<size_t>(-1)) {
        index_ = fdes_.size();
      }
    }

    iterator& operator++() {
      index_++;
      return *this;
    }
    iterator& operator++(int increment) {
      index_ += increment;
      return *this;
    }
    iterator& operator--() {
      index_--;
      return *this;
    }
    iterator& operator--(int decrement) {
      index_ -= decrement;
      return *this;
    }

    bool operator==(const iterator& rhs) { return this->index_ == rhs.index_; }
    bool operator!=(const iterator& rhs) { return this->index_ != rhs.index_; }

    const DwarfFde* operator*() {
      if (index_ > fdes_.size()) return nullptr;
      return fdes_[index_];
    }

   private:
    std::vector<const DwarfFde*> fdes_;
    size_t index_ = 0;
  };

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, static_cast<size_t>(-1)); }

  DwarfErrorCode LastErrorCode() { return last_error_.code; }
  uint64_t LastErrorAddress() { return last_error_.address; }

  virtual bool Init(uint64_t offset, uint64_t size, int64_t section_bias) = 0;

  virtual bool Eval(const DwarfCie*, Memory*, const dwarf_loc_regs_t&, Regs*, bool*) = 0;

  virtual bool Log(uint8_t indent, uint64_t pc, const DwarfFde* fde, ArchEnum arch) = 0;

  virtual void GetFdes(std::vector<const DwarfFde*>* fdes) = 0;

  virtual const DwarfFde* GetFdeFromPc(uint64_t pc) = 0;

  virtual bool GetCfaLocationInfo(uint64_t pc, const DwarfFde* fde, dwarf_loc_regs_t* loc_regs,
                                  ArchEnum arch) = 0;

  virtual uint64_t GetCieOffsetFromFde32(uint32_t pointer) = 0;

  virtual uint64_t GetCieOffsetFromFde64(uint64_t pointer) = 0;

  virtual uint64_t AdjustPcFromFde(uint64_t pc) = 0;

  bool Step(uint64_t pc, Regs* regs, Memory* process_memory, bool* finished, bool* is_signal_frame);

 protected:
  DwarfMemory memory_;
  DwarfErrorData last_error_{DWARF_ERROR_NONE, 0};

  uint32_t cie32_value_ = 0;
  uint64_t cie64_value_ = 0;

  std::unordered_map<uint64_t, DwarfFde> fde_entries_;
  std::unordered_map<uint64_t, DwarfCie> cie_entries_;
  std::unordered_map<uint64_t, dwarf_loc_regs_t> cie_loc_regs_;
  std::map<uint64_t, dwarf_loc_regs_t> loc_regs_;  // Single row indexed by pc_end.
};

template <typename AddressType>
class DwarfSectionImpl : public DwarfSection {
 public:
  DwarfSectionImpl(Memory* memory) : DwarfSection(memory) {}
  virtual ~DwarfSectionImpl() = default;

  bool Init(uint64_t offset, uint64_t size, int64_t section_bias) override;

  const DwarfCie* GetCieFromOffset(uint64_t offset);

  const DwarfFde* GetFdeFromOffset(uint64_t offset);

  const DwarfFde* GetFdeFromPc(uint64_t pc) override;

  void GetFdes(std::vector<const DwarfFde*>* fdes) override;

  bool EvalRegister(const DwarfLocation* loc, uint32_t reg, AddressType* reg_ptr, void* info);

  bool Eval(const DwarfCie* cie, Memory* regular_memory, const dwarf_loc_regs_t& loc_regs,
            Regs* regs, bool* finished) override;

  bool GetCfaLocationInfo(uint64_t pc, const DwarfFde* fde, dwarf_loc_regs_t* loc_regs,
                          ArchEnum arch) override;

  bool Log(uint8_t indent, uint64_t pc, const DwarfFde* fde, ArchEnum arch) override;

 protected:
  bool GetNextCieOrFde(const DwarfFde** fde_entry);

  bool FillInCieHeader(DwarfCie* cie);

  bool FillInCie(DwarfCie* cie);

  bool FillInFdeHeader(DwarfFde* fde);

  bool FillInFde(DwarfFde* fde);

  bool EvalExpression(const DwarfLocation& loc, Memory* regular_memory, AddressType* value,
                      RegsInfo<AddressType>* regs_info, bool* is_dex_pc);

  void InsertFde(const DwarfFde* fde);

  int64_t section_bias_ = 0;
  uint64_t entries_offset_ = 0;
  uint64_t entries_end_ = 0;
  uint64_t next_entries_offset_ = 0;
  uint64_t pc_offset_ = 0;

  std::map<uint64_t, std::pair<uint64_t, const DwarfFde*>> fdes_;
};

}  // namespace unwindstack

#endif  // _LIBUNWINDSTACK_DWARF_SECTION_H
