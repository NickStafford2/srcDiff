// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file shortest_edit_script.cpp
 *
 * @copyright Copyright (C) 2014-2024 SDML (www.srcDiff.org)
 *
 * This file is part of the srcDiff Infrastructure.
 */

#include <shortest_edit_script.hpp>

namespace ses {

const size_t shortest_edit_script::SIZE_THRESHOLD = 20480;
shortest_edit_script::shortest_edit_script(int (*compare)(const void* item_one, const void* item_two, const void* context),
                                               const void* (*accessor)(int index, const void* structure, const void* context),
                                               const void* context,
                                               int threshold)
    : context(context),
      compare(compare),
      accessor(accessor),
      threshold(threshold) {
}

shortest_edit_script::~shortest_edit_script() {
}

size_t shortest_edit_script::get_size_threshold() {
  return SIZE_THRESHOLD;
}

edit_list shortest_edit_script::compute(const void* structure_one, int size_one, const void* structure_two, int size_two) {
  return compute_impl(structure_one, size_one, structure_two, size_two, nullptr);
}

edit_list shortest_edit_script::compute_profiled(const void* structure_one, int size_one, const void* structure_two, int size_two,
                                                 struct ses_profile* profile) {
  return compute_impl(structure_one, size_one, structure_two, size_two, profile);
}

edit_list shortest_edit_script::compute_impl(const void* structure_one, int size_one, const void* structure_two, int size_two,
                                             struct ses_profile* profile) {
  edit_t* edit_script = nullptr;
  int distance = shortest_edit_script_hybrid_profiled(structure_one, size_one, structure_two, size_two, &edit_script,
                                     *compare.target<int (*)(void const*, void const*, void const*)>(),
                                     *accessor.target<void const* (*)(int, void const*, void const*)>(),
                                     context, threshold, profile);
  if(distance < 0) throw std::logic_error("Error computing shortest edit script");

  if(profile && profile->record_event) profile->record_event(profile->context, SES_PROFILE_RECONSTRUCTION_BEGIN);
  edit_list edits;
  for(edit_t* edit = edit_script; edit != nullptr; edit = edit->next) {
    if(is_change(edit)) {
      edit_t* next = edit->next;
      edits.emplace_back(ses::CHANGE,
                         edit->offset_sequence_one, edit->length,
                         next->offset_sequence_two, next->length
      );
      edit = next;
    } else{
      edits.emplace_back(edit->operation == SES_DELETE ? ses::DELETE : ses::INSERT,
                         edit->offset_sequence_one, edit->operation == SES_DELETE ? edit->length : 0,
                         edit->offset_sequence_two, edit->operation == SES_INSERT ? edit->length : 0
      );
    }
  }

  free_shortest_edit_script(edit_script);
  if(profile && profile->record_event) profile->record_event(profile->context, SES_PROFILE_RECONSTRUCTION_END);

  return edits;
}

}
