// SPDX-License-Identifier: GPL-3.0-only
#ifndef INCLUDED_PROFILE_HPP
#define INCLUDED_PROFILE_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>

namespace srcdiff {

enum class profile_phase {
  input_original,
  input_modified,
  delta,
  serialization,
  archive_write,
  structural_ses,
  edit_correction,
  move_detection,
  change_matcher,
  nesting,
  text_ses_important,
  text_ses_complete,
  syntax_ses
};

class profiler {
public:
  using clock = std::chrono::steady_clock;

  static profiler& instance() {
    static profiler value;
    return value;
  }

  void enable() { enabled_ = true; }
  bool enabled() const { return enabled_; }

  void add(profile_phase phase, clock::duration elapsed) {
    const double milliseconds =
      std::chrono::duration<double, std::milli>(elapsed).count();
    switch(phase) {
      case profile_phase::input_original: input_original_ms_ += milliseconds; break;
      case profile_phase::input_modified: input_modified_ms_ += milliseconds; break;
      case profile_phase::delta: delta_inclusive_ms_ += milliseconds; break;
      case profile_phase::serialization: serialization_ms_ += milliseconds; break;
      case profile_phase::archive_write: archive_write_ms_ += milliseconds; break;
      case profile_phase::structural_ses: structural_ses_ms_ += milliseconds; break;
      case profile_phase::edit_correction: edit_correction_ms_ += milliseconds; break;
      case profile_phase::move_detection: move_detection_ms_ += milliseconds; break;
      case profile_phase::change_matcher: change_matcher_ms_ += milliseconds; break;
      case profile_phase::nesting: nesting_ms_ += milliseconds; break;
      case profile_phase::text_ses_important:
        text_ses_ms_ += milliseconds;
        text_ses_important_ms_ += milliseconds;
        break;
      case profile_phase::text_ses_complete:
        text_ses_ms_ += milliseconds;
        text_ses_complete_ms_ += milliseconds;
        break;
      case profile_phase::syntax_ses: syntax_ses_ms_ += milliseconds; break;
    }
  }

  void file_processed() { ++files_processed_; }
  void top_level_constructs(std::size_t original, std::size_t modified) {
    top_level_original_ += original;
    top_level_modified_ += modified;
  }
  void change_matrix(std::size_t cells, std::size_t bytes) {
    ++change_matcher_calls_;
    change_matrix_cells_ += cells;
    change_matrix_bytes_ += bytes;
    change_matrix_max_cells_ = std::max(change_matrix_max_cells_, cells);
  }
  void text_ses_call(bool important_only, std::size_t original_terms, std::size_t modified_terms) {
    ++text_ses_calls_;
    text_ses_original_terms_ += original_terms;
    text_ses_modified_terms_ += modified_terms;
    text_ses_max_cells_ = std::max(text_ses_max_cells_, original_terms * modified_terms);
    if(important_only) {
      ++text_ses_important_calls_;
      text_ses_important_original_terms_ += original_terms;
      text_ses_important_modified_terms_ += modified_terms;
      text_ses_important_max_cells_ =
        std::max(text_ses_important_max_cells_, original_terms * modified_terms);
    } else {
      ++text_ses_complete_calls_;
      text_ses_complete_original_terms_ += original_terms;
      text_ses_complete_modified_terms_ += modified_terms;
      text_ses_complete_max_cells_ =
        std::max(text_ses_complete_max_cells_, original_terms * modified_terms);
    }
  }
  void finish_text_ses(bool important_only,
                       std::size_t original_terms,
                       std::size_t modified_terms,
                       std::size_t edit_distance,
                       clock::duration elapsed) {
    add(important_only ? profile_phase::text_ses_important
                       : profile_phase::text_ses_complete,
        elapsed);
    if(important_only) return;

    const double milliseconds =
      std::chrono::duration<double, std::milli>(elapsed).count();
    if(milliseconds <= text_ses_complete_max_ms_) return;

    text_ses_complete_max_ms_ = milliseconds;
    text_ses_complete_max_call_ = text_ses_complete_calls_;
    text_ses_complete_max_original_terms_ = original_terms;
    text_ses_complete_max_modified_terms_ = modified_terms;
    text_ses_complete_max_edit_distance_ = edit_distance;
  }
  void syntax_ses_call() { ++syntax_ses_calls_; }
  void measure_cache_hit() { ++measure_cache_hits_; }
  void measure_cache_miss() { ++measure_cache_misses_; }
  void nesting_check() { ++nesting_checks_; }

  void report(clock::duration total) const {
    if(!enabled_) return;

    const double total_ms =
      std::chrono::duration<double, std::milli>(total).count();
    const double delta_ms =
      std::max(0.0, delta_inclusive_ms_ - serialization_ms_);

    std::cerr << std::fixed << std::setprecision(3)
              << "profile.pipeline.total_ms=" << total_ms << '\n'
              << "profile.input.original_ms=" << input_original_ms_ << '\n'
              << "profile.input.modified_ms=" << input_modified_ms_ << '\n'
              << "profile.delta.total_ms=" << delta_ms << '\n'
              << "profile.serialization_ms=" << serialization_ms_ << '\n'
              << "profile.archive_write_ms=" << archive_write_ms_ << '\n'
              << "profile.files_processed=" << files_processed_ << '\n'
              << "profile.delta.structural_ses_ms=" << structural_ses_ms_ << '\n'
              << "profile.delta.edit_correction_ms=" << edit_correction_ms_ << '\n'
              << "profile.delta.move_detection_ms=" << move_detection_ms_ << '\n'
              << "profile.delta.change_matcher_ms=" << change_matcher_ms_ << '\n'
              << "profile.delta.nesting_ms=" << nesting_ms_ << '\n'
              << "profile.delta.text_ses_ms=" << text_ses_ms_ << '\n'
              << "profile.delta.text_ses_important_ms=" << text_ses_important_ms_ << '\n'
              << "profile.delta.text_ses_complete_ms=" << text_ses_complete_ms_ << '\n'
              << "profile.delta.syntax_ses_ms=" << syntax_ses_ms_ << '\n'
              << "profile.delta.top_level_original=" << top_level_original_ << '\n'
              << "profile.delta.top_level_modified=" << top_level_modified_ << '\n'
              << "profile.delta.change_matcher_calls=" << change_matcher_calls_ << '\n'
              << "profile.delta.change_matrix_cells=" << change_matrix_cells_ << '\n'
              << "profile.delta.change_matrix_max_cells=" << change_matrix_max_cells_ << '\n'
              << "profile.delta.change_matrix_bytes=" << change_matrix_bytes_ << '\n'
              << "profile.delta.text_ses_calls=" << text_ses_calls_ << '\n'
              << "profile.delta.text_ses_original_terms=" << text_ses_original_terms_ << '\n'
              << "profile.delta.text_ses_modified_terms=" << text_ses_modified_terms_ << '\n'
              << "profile.delta.text_ses_max_cells=" << text_ses_max_cells_ << '\n'
              << "profile.delta.text_ses_important_calls=" << text_ses_important_calls_ << '\n'
              << "profile.delta.text_ses_important_original_terms=" << text_ses_important_original_terms_ << '\n'
              << "profile.delta.text_ses_important_modified_terms=" << text_ses_important_modified_terms_ << '\n'
              << "profile.delta.text_ses_important_max_cells=" << text_ses_important_max_cells_ << '\n'
              << "profile.delta.text_ses_complete_calls=" << text_ses_complete_calls_ << '\n'
              << "profile.delta.text_ses_complete_original_terms=" << text_ses_complete_original_terms_ << '\n'
              << "profile.delta.text_ses_complete_modified_terms=" << text_ses_complete_modified_terms_ << '\n'
              << "profile.delta.text_ses_complete_max_cells=" << text_ses_complete_max_cells_ << '\n'
              << "profile.delta.text_ses_complete_max_ms=" << text_ses_complete_max_ms_ << '\n'
              << "profile.delta.text_ses_complete_max_call=" << text_ses_complete_max_call_ << '\n'
              << "profile.delta.text_ses_complete_max_original_terms=" << text_ses_complete_max_original_terms_ << '\n'
              << "profile.delta.text_ses_complete_max_modified_terms=" << text_ses_complete_max_modified_terms_ << '\n'
              << "profile.delta.text_ses_complete_max_edit_distance=" << text_ses_complete_max_edit_distance_ << '\n'
              << "profile.delta.syntax_ses_calls=" << syntax_ses_calls_ << '\n'
              << "profile.delta.measure_cache_hits=" << measure_cache_hits_ << '\n'
              << "profile.delta.measure_cache_misses=" << measure_cache_misses_ << '\n'
              << "profile.delta.nesting_checks=" << nesting_checks_ << '\n';
  }

private:
  bool enabled_ = false;
  double input_original_ms_ = 0.0;
  double input_modified_ms_ = 0.0;
  double delta_inclusive_ms_ = 0.0;
  double serialization_ms_ = 0.0;
  double archive_write_ms_ = 0.0;
  double structural_ses_ms_ = 0.0;
  double edit_correction_ms_ = 0.0;
  double move_detection_ms_ = 0.0;
  double change_matcher_ms_ = 0.0;
  double nesting_ms_ = 0.0;
  double text_ses_ms_ = 0.0;
  double text_ses_important_ms_ = 0.0;
  double text_ses_complete_ms_ = 0.0;
  double syntax_ses_ms_ = 0.0;
  std::size_t files_processed_ = 0;
  std::size_t top_level_original_ = 0;
  std::size_t top_level_modified_ = 0;
  std::size_t change_matcher_calls_ = 0;
  std::size_t change_matrix_cells_ = 0;
  std::size_t change_matrix_max_cells_ = 0;
  std::size_t change_matrix_bytes_ = 0;
  std::size_t text_ses_calls_ = 0;
  std::size_t text_ses_original_terms_ = 0;
  std::size_t text_ses_modified_terms_ = 0;
  std::size_t text_ses_max_cells_ = 0;
  std::size_t text_ses_important_calls_ = 0;
  std::size_t text_ses_important_original_terms_ = 0;
  std::size_t text_ses_important_modified_terms_ = 0;
  std::size_t text_ses_important_max_cells_ = 0;
  std::size_t text_ses_complete_calls_ = 0;
  std::size_t text_ses_complete_original_terms_ = 0;
  std::size_t text_ses_complete_modified_terms_ = 0;
  std::size_t text_ses_complete_max_cells_ = 0;
  double text_ses_complete_max_ms_ = 0.0;
  std::size_t text_ses_complete_max_call_ = 0;
  std::size_t text_ses_complete_max_original_terms_ = 0;
  std::size_t text_ses_complete_max_modified_terms_ = 0;
  std::size_t text_ses_complete_max_edit_distance_ = 0;
  std::size_t syntax_ses_calls_ = 0;
  std::size_t measure_cache_hits_ = 0;
  std::size_t measure_cache_misses_ = 0;
  std::size_t nesting_checks_ = 0;
};

class profile_timer {
public:
  explicit profile_timer(profile_phase phase)
    : phase_(phase), active_(profiler::instance().enabled()) {
    if(active_) start_ = profiler::clock::now();
  }

  ~profile_timer() {
    if(active_) profiler::instance().add(phase_, profiler::clock::now() - start_);
  }

  profile_timer(const profile_timer&) = delete;
  profile_timer& operator=(const profile_timer&) = delete;

private:
  profile_phase phase_;
  bool active_;
  profiler::clock::time_point start_;
};

}

#endif
